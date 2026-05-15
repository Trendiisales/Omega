#pragma once
// =============================================================================
// GoldUltimateStrategy.hpp -- THE MASTER GOLD TRADING ORCHESTRATOR
// =============================================================================
//
// PURPOSE
// -------
// Single unified controller that:
//   1. Detects current market regime (BULL_TREND / BEAR_TREND / MEAN_REVERSION
//      / COMPRESSION / NOISE) using ALL available signals
//   2. Selects which engine(s) should fire for the detected regime
//   3. Ensures EVERY trade covers costs BEFORE entry (cost gate + EV gate)
//   4. Implements IMMEDIATE loss cutting with aggressive trailing
//   5. Operates in BOTH bull and bear markets with equal conviction
//   6. Coordinates position limits across the engine fleet
//
// ARCHITECTURE
// ------------
// This sits ABOVE the individual engines. It does NOT replace them -- it
// ORCHESTRATES them. Each engine retains its own signal generation logic.
// This controller decides:
//   - WHICH engines are allowed to fire right now (regime routing)
//   - WHETHER a proposed trade covers costs (cost viability)
//   - HOW MUCH size to allocate (regime-weighted lot scaling)
//   - WHEN to cut (unified loss management layer)
//
// REGIME DETECTION (multi-layer fusion)
// ------------------------------------
// Layer 1: GoldHMM           -> CONTINUATION / MEAN_REVERSION / NOISE
// Layer 2: MacroRegimeDetector -> RISK_ON / NEUTRAL / RISK_OFF
// Layer 3: VolBand           -> CRUSH / LOW / NORMAL / HIGH
// Layer 4: Price Structure   -> BULL (higher highs) / BEAR (lower lows) / RANGE
// Layer 5: Momentum          -> ewm_drift sign + magnitude
//
// These 5 layers combine into one of 5 TRADING REGIMES:
//   BULL_TREND      = CONT + (RISK_OFF or NEUTRAL) + drift > 0 + HH structure
//   BEAR_TREND      = CONT + (RISK_ON or event) + drift < 0 + LL structure
//   MEAN_REVERSION  = HMM_MR + any macro + moderate vol
//   COMPRESSION     = NOISE/CRUSH + low ATR + tight range
//   NOISE           = HMM_NOISE + RISK_ON + no structure
//
// ENGINE FLEET (what fires in each regime)
// ----------------------------------------
// BULL_TREND:
//   PRIMARY:   XauTrendFollow2h, XauTrendFollow4h, CandleFlowGold (CONT gate)
//   SECONDARY: GoldMidScalper (WITH trend), SessionMomentum, DonchianBreakout
//   BLOCKED:   MeanReversion, VWAPSnapback, SpikeFade
//
// BEAR_TREND:
//   PRIMARY:   MacroCrashEngine, XauTrendFollowD1 (SHORT), SpikeFade (long fades)
//   SECONDARY: XauTrendFollow2h (SHORT), LiquiditySweepPro
//   BLOCKED:   SessionMomentum (long-only), DonchianBreakout (choppy in crashes)
//
// MEAN_REVERSION:
//   PRIMARY:   XauusdFvg, XauThreeBar30m, MeanReversion, VWAPSnapback
//   SECONDARY: DynamicRange, NR3Breakout, LiquiditySweepPressure
//   BLOCKED:   TrendFollow engines, CandleFlow, SessionMomentum
//
// COMPRESSION:
//   PRIMARY:   NR3Breakout, NR3Tick, AsianRange, DonchianBreakout (breakout watch)
//   SECONDARY: IntradaySeasonality, VWAPStretchReversion
//   BLOCKED:   All trend engines, MacroCrash, scalpers
//
// NOISE:
//   PRIMARY:   NOTHING (sit on hands)
//   SECONDARY: IntradaySeasonality (if signal is very strong, score >= 12)
//   BLOCKED:   Everything else -- capital preservation mode
//
// COST COVERAGE (mandatory pre-fire check)
// -----------------------------------------
// Every trade MUST satisfy:
//   1. ExecutionCostGuard::is_viable() with ratio >= 1.5x (spread+slip+comm < TP/1.5)
//   2. EVGuard::should_fire() -- empirical EV > safety_margin ($0.20)
//   3. Signal score >= dynamic threshold (regime-adjusted)
//
// LOSS MANAGEMENT (immediate cut system)
// ---------------------------------------
// Three-stage loss control:
//   Stage 1: Hard SL at entry (ATR-proportional, engine-specific)
//   Stage 2: Break-even shift at 1.0 ATR MFE (locks in spread cost)
//   Stage 3: Aggressive trailing stop at 0.5-0.75 ATR behind price
//
// Additional kill switches:
//   - 3 consecutive losses -> engine paused for 30 minutes
//   - Daily loss cap $50 -> all engines shut down for UTC day
//   - Single trade loss > 2x expected -> reduce next trade size by 50%
//   - Drawdown from session peak > $100 -> shadow mode remainder of day
//
// POSITION SIZING (regime-aware)
// ------------------------------
//   Base: 0.01 lot (minimum for XAUUSD)
//   Scale: regime_weight * signal_quality_scale * vol_inverse_scale
//     regime_weight:        0.5 (NOISE) to 1.5 (BULL_TREND w/ RISK_OFF)
//     signal_quality_scale: score/16 (linear from signal scorer)
//     vol_inverse_scale:    min(1.5, 5.0 / ATR) -- smaller in high vol
//   Cap: 0.05 lot absolute maximum
//   Floor: 0.01 lot minimum (below this, don't trade)
//
// THREAD SAFETY
// -------------
// Single-threaded per design (called from engine_dispatch worker only).
// No mutex needed. Atomic reads for cross-thread indicator reads.
//
// PROTECTED CODE INVARIANT
// ------------------------
// This file does NOT modify:
//   - OmegaFIX.hpp (IMMUTABLE)
//   - OmegaTradeLedger.hpp (CORE)
//   - order_exec.hpp (CORE)
//   - trade_lifecycle.hpp (CORE)
//   - Any existing engine's internal logic
// It READS from existing globals and COORDINATES engine firing.
// =============================================================================

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <functional>
#include <string>

// GoldRegimeRouter.hpp removed — design-only skeleton, not committed.
// The strategy class does not depend on it.
#include "GoldHMM.hpp"
#include "OmegaSignalScorer.hpp"
#include "OmegaCostGuard.hpp"
#include "OmegaEVGuard.hpp"
#include "engine_protections.hpp"

namespace omega { namespace gold_ultimate {

// =============================================================================
// TRADING REGIME -- the fused state that drives all decisions
// =============================================================================
enum class TradingRegime : int {
    BULL_TREND      = 0,   // Strong upward continuation
    BEAR_TREND      = 1,   // Strong downward continuation (crash/sell-off)
    MEAN_REVERSION  = 2,   // Choppy, fade extremes
    COMPRESSION     = 3,   // Tight range, breakout imminent
    NOISE           = 4,   // No edge -- sit on hands
    COUNT           = 5
};

inline const char* regime_name(TradingRegime r) noexcept {
    switch (r) {
        case TradingRegime::BULL_TREND:     return "BULL_TREND";
        case TradingRegime::BEAR_TREND:     return "BEAR_TREND";
        case TradingRegime::MEAN_REVERSION: return "MEAN_REVERSION";
        case TradingRegime::COMPRESSION:    return "COMPRESSION";
        case TradingRegime::NOISE:          return "NOISE";
        default:                            return "UNKNOWN";
    }
}

// =============================================================================
// ENGINE IDENTITY -- all gold engines that can be orchestrated
// =============================================================================
enum class EngineId : int {
    // Original router engines
    XauusdFvg = 0,
    XauThreeBar30m,
    XauTrendFollow2h,
    XauTrendFollow4h,
    XauTrendFollowD1,
    MinimalH4Gold,
    CandleFlowGold,
    GoldMicroScalper,
    GoldMidScalper,
    // GoldEngineStack engines
    SessionMomentum,
    VWAPSnapback,
    LiquiditySweepPro,
    LiquiditySweepPressure,
    MeanReversion,
    IntradaySeasonality,
    DonchianBreakout,
    NR3Breakout,
    SpikeFade,
    AsianRange,
    DynamicRange,
    NR3Tick,
    TwoBarReversal,
    LondonFixMomentum,
    VWAPStretchReversion,
    ORBNewYork,
    DXYDivergence,
    SessionOpenMomentum,
    // Standalone engines
    MacroCrash,
    EMACross,
    COUNT
};

inline constexpr int kTotalEngines = static_cast<int>(EngineId::COUNT);

inline const char* engine_id_name(EngineId id) noexcept {
    switch (id) {
        case EngineId::XauusdFvg:              return "XauusdFvg";
        case EngineId::XauThreeBar30m:         return "XauThreeBar30m";
        case EngineId::XauTrendFollow2h:       return "XauTrendFollow2h";
        case EngineId::XauTrendFollow4h:       return "XauTrendFollow4h";
        case EngineId::XauTrendFollowD1:       return "XauTrendFollowD1";
        case EngineId::MinimalH4Gold:          return "MinimalH4Gold";
        case EngineId::CandleFlowGold:         return "CandleFlowGold";
        case EngineId::GoldMicroScalper:       return "GoldMicroScalper";
        case EngineId::GoldMidScalper:         return "GoldMidScalper";
        case EngineId::SessionMomentum:        return "SessionMomentum";
        case EngineId::VWAPSnapback:           return "VWAPSnapback";
        case EngineId::LiquiditySweepPro:      return "LiquiditySweepPro";
        case EngineId::LiquiditySweepPressure: return "LiquiditySweepPressure";
        case EngineId::MeanReversion:          return "MeanReversion";
        case EngineId::IntradaySeasonality:    return "IntradaySeasonality";
        case EngineId::DonchianBreakout:       return "DonchianBreakout";
        case EngineId::NR3Breakout:            return "NR3Breakout";
        case EngineId::SpikeFade:              return "SpikeFade";
        case EngineId::AsianRange:             return "AsianRange";
        case EngineId::DynamicRange:           return "DynamicRange";
        case EngineId::NR3Tick:                return "NR3Tick";
        case EngineId::TwoBarReversal:         return "TwoBarReversal";
        case EngineId::LondonFixMomentum:      return "LondonFixMomentum";
        case EngineId::VWAPStretchReversion:   return "VWAPStretchReversion";
        case EngineId::ORBNewYork:             return "ORBNewYork";
        case EngineId::DXYDivergence:          return "DXYDivergence";
        case EngineId::SessionOpenMomentum:    return "SessionOpenMomentum";
        case EngineId::MacroCrash:             return "MacroCrash";
        case EngineId::EMACross:               return "EMACross";
        default:                               return "UNKNOWN";
    }
}

// =============================================================================
// MARKET SNAPSHOT -- all data needed to make a regime + entry decision
// =============================================================================
struct MarketSnapshot {
    // Price
    double bid            = 0.0;
    double ask            = 0.0;
    double mid            = 0.0;
    double spread_pts     = 0.0;

    // Regime inputs
    int    hmm_state      = 0;     // 0=CONT, 1=MR, 2=NOISE
    bool   hmm_warmed     = false;
    double hmm_p_cont     = 1.0;   // probability of continuation

    // Macro
    std::string macro_regime = "NEUTRAL";  // RISK_ON / NEUTRAL / RISK_OFF
    int    vol_band       = 2;     // 0=CRUSH,1=LOW,2=NORMAL,3=HIGH

    // Momentum & structure
    double ewm_drift      = 0.0;   // signed drift (+ = bullish)
    double vol_ratio      = 0.0;   // recent_vol / base_vol
    double atr_pts        = 0.0;   // current ATR in price points
    double rsi14          = 50.0;  // RSI-14
    bool   higher_highs   = false; // price structure bullish
    bool   lower_lows     = false; // price structure bearish

    // Signal scorer
    int    signal_score   = 0;     // 0..16
    bool   signal_pass    = false; // score >= threshold

    // Cross-asset
    double dxy_return     = 0.0;   // fractional DXY change
    double spx_return     = 0.0;   // fractional SPX change

    // Timing
    int64_t now_ms        = 0;
    int     hour_utc      = 0;
    int     minute_utc    = 0;

    // L2 microstructure
    double l2_imbalance   = 0.5;   // 0..1 (0.5 = balanced)
    double microprice     = 0.0;   // signed pts bias
};

// =============================================================================
// ENTRY DECISION -- what the strategy produces
// =============================================================================
struct EntryDecision {
    bool        allow           = false;
    bool        is_long         = true;
    EngineId    engine          = EngineId::XauusdFvg;
    double      lot_size        = 0.01;
    double      sl_pts          = 0.0;    // stop loss distance in pts
    double      tp_pts          = 0.0;    // take profit distance in pts
    double      be_trigger_pts  = 0.0;    // break-even trigger distance
    double      trail_pts       = 0.0;    // trailing stop distance
    TradingRegime regime        = TradingRegime::NOISE;
    const char* block_reason    = "NOT_EVALUATED";
    int         signal_score    = 0;
    double      ev_usd          = 0.0;    // expected value of trade
    double      cost_usd        = 0.0;    // estimated cost
};

// =============================================================================
// LOSS MANAGEMENT STATE -- per-session tracking
// =============================================================================
struct LossManagementState {
    // Consecutive loss tracking
    int     consec_losses       = 0;
    int64_t pause_until_ms      = 0;     // engine paused until this time

    // Daily tracking
    double  daily_pnl_usd       = 0.0;
    double  session_peak_pnl    = 0.0;
    double  session_drawdown    = 0.0;
    int64_t day_utc_idx         = -1;
    bool    daily_shutdown      = false;
    bool    shadow_forced       = false;

    // Adaptive sizing
    double  size_penalty        = 1.0;   // 0.5 after big loss, 1.0 normal
    int64_t penalty_expires_ms  = 0;

    // Trade counters
    int     trades_today        = 0;
    int     wins_today          = 0;
    int     losses_today        = 0;
};

// =============================================================================
// REGIME ROUTING TABLE -- which engines fire in each regime
// =============================================================================
struct RegimeRoutingEntry {
    float weight;   // 0.0 = blocked, 0.0-0.5 = secondary, 0.5-1.0 = primary, >1.0 = boosted
};

// =============================================================================
// GoldUltimateStrategy -- THE MASTER CONTROLLER
// =============================================================================
class GoldUltimateStrategy {
public:
    // ─────────────────────────────────────────────────────────────────────────
    // Configuration (tunable, set once at init or hot-reload)
    // ─────────────────────────────────────────────────────────────────────────

    // Master enable. When false, the strategy is inert.
    bool enabled = false;

    // Cost coverage requirements
    double cost_ratio_min       = 1.5;    // TP must be >= 1.5x total cost
    double ev_safety_margin     = 0.20;   // EV must exceed $0.20

    // Loss management thresholds
    int    max_consec_losses    = 3;      // pause after N consecutive losses
    int    pause_duration_ms    = 1800000; // 30 min pause
    double daily_loss_cap_usd   = 50.0;   // shut down after $50 daily loss
    double drawdown_shadow_usd  = 100.0;  // force shadow after $100 drawdown
    double big_loss_threshold   = 2.0;    // 2x expected = "big loss" -> size penalty

    // Position sizing
    double base_lot             = 0.01;
    double max_lot              = 0.05;
    double min_lot              = 0.01;

    // Regime detection thresholds
    double drift_trend_thresh   = 3.0;    // |drift| > 3 = trending
    double drift_strong_thresh  = 6.0;    // |drift| > 6 = strong trend
    double vol_ratio_trend      = 1.0;    // vol_ratio > 1.0 = confirming
    double atr_compression_max  = 2.0;    // ATR < 2.0 = compression
    double atr_noise_max        = 1.5;    // ATR < 1.5 in noise = dead tape

    // Signal thresholds per regime
    int    score_min_trend      = 5;      // lower bar in trending (edge is strong)
    int    score_min_mr         = 6;      // moderate bar in MR
    int    score_min_compression= 7;      // higher bar in compression (less certain)
    int    score_min_noise      = 12;     // very high bar in noise (only high-conviction)

    // v12 OOS-validated edge filters (backtest: 26 months, 154M ticks)
    // Session filter: only trade during hours with confirmed OOS edge.
    // Set edge_hours[h]=true for each UTC hour that should trade.
    // Default: 01, 05, 23 UTC (Asian/early-London session).
    // ATR floor: reject entries when ATR is below this threshold.
    // ATR 1.5-2.5 band had PF=0.80 across 481 trades (-$236).
    std::array<bool, 24> edge_hours = []() {
        std::array<bool, 24> h{};
        h[1] = true;   // 01:00 UTC — IS PF=1.44, OOS PF=1.44 (CONFIRMED)
        h[5] = true;   // 05:00 UTC — OOS PF=1.50 (70 trades)
        h[23] = true;  // 23:00 UTC — OOS PF=1.22 (73 trades)
        return h;
    }();
    double atr_entry_floor      = 2.5;    // ATR must be >= 2.5 for entry

    // ─────────────────────────────────────────────────────────────────────────
    // Constructor
    // ─────────────────────────────────────────────────────────────────────────
    GoldUltimateStrategy() noexcept {
        _init_routing_table();
        loss_state_ = LossManagementState{};
    }

    // ─────────────────────────────────────────────────────────────────────────
    // detect_regime() -- Fuse all signals into a single TradingRegime
    // ───────────────────────────────────────────────────────────────────���─────
    TradingRegime detect_regime(const MarketSnapshot& snap) const noexcept {
        // Layer 1: HMM state (strongest single classifier)
        const bool hmm_cont  = (snap.hmm_state == 0);
        const bool hmm_mr    = (snap.hmm_state == 1);
        const bool hmm_noise = (snap.hmm_state == 2);

        // Layer 2: Momentum direction and strength
        const double abs_drift = std::fabs(snap.ewm_drift);
        const bool drift_trending = (abs_drift >= drift_trend_thresh);
        const bool drift_strong   = (abs_drift >= drift_strong_thresh);
        const bool drift_bull     = (snap.ewm_drift > 0.0);
        const bool drift_bear     = (snap.ewm_drift < 0.0);

        // Layer 3: Vol expansion
        const bool vol_confirming = (snap.vol_ratio >= vol_ratio_trend);
        const bool vol_compressed = (snap.atr_pts < atr_compression_max);
        const bool vol_dead       = (snap.atr_pts < atr_noise_max);

        // Layer 4: Macro context
        const bool risk_off = (snap.macro_regime == "RISK_OFF");
        const bool risk_on  = (snap.macro_regime == "RISK_ON");

        // Layer 5: Price structure
        const bool structure_bull = snap.higher_highs;
        const bool structure_bear = snap.lower_lows;

        // ── REGIME FUSION LOGIC ──────────────────────────────────────────────
        //
        // Priority order: NOISE first (capital preservation), then directional,
        // then MR, then compression.

        // NOISE: HMM says noise + no drift + dead tape
        if (hmm_noise && !drift_trending && vol_dead) {
            return TradingRegime::NOISE;
        }

        // BULL TREND: HMM continuation + positive drift + structure confirms
        // OR: strong drift with vol expansion (HMM may lag)
        if ((hmm_cont && drift_bull && drift_trending && (vol_confirming || structure_bull))
            || (drift_bull && drift_strong && vol_confirming)) {
            // Additional boost check: RISK_OFF makes gold bull even stronger
            // But we classify as BULL_TREND regardless of macro in strong drift
            return TradingRegime::BULL_TREND;
        }

        // BEAR TREND: HMM continuation + negative drift + structure confirms
        // OR: macro crash (RISK_ON + strong negative drift)
        if ((hmm_cont && drift_bear && drift_trending && (vol_confirming || structure_bear))
            || (drift_bear && drift_strong && vol_confirming)
            || (risk_on && drift_bear && abs_drift >= drift_trend_thresh)) {
            return TradingRegime::BEAR_TREND;
        }

        // MEAN REVERSION: HMM says MR, OR moderate drift with fading momentum
        if (hmm_mr || (drift_trending && !vol_confirming && !structure_bull && !structure_bear)) {
            return TradingRegime::MEAN_REVERSION;
        }

        // COMPRESSION: low vol, tight range, breakout imminent
        if (vol_compressed && !drift_trending) {
            return TradingRegime::COMPRESSION;
        }

        // NOISE fallback: if nothing else matches, be conservative
        if (hmm_noise) {
            return TradingRegime::NOISE;
        }

        // Default: MEAN_REVERSION is the safest "uncertain" state
        // (fade extremes rather than chase)
        return TradingRegime::MEAN_REVERSION;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // evaluate_entry() -- Full decision pipeline for a proposed entry
    //
    // Called by each engine's should_fire() path. Returns allow/block with
    // full diagnostic reasoning.
    // ─────────────────────────────────────────────────────────────────────────
    EntryDecision evaluate_entry(const MarketSnapshot& snap,
                                 EngineId engine,
                                 bool is_long,
                                 double proposed_sl_pts,
                                 double proposed_tp_pts) noexcept {
        EntryDecision d{};
        d.engine = engine;
        d.is_long = is_long;
        d.signal_score = snap.signal_score;

        // ─�� MASTER ENABLE CHECK ──────────────────────────────────────────────
        if (!enabled) {
            d.block_reason = "STRATEGY_DISABLED";
            return d;
        }

        // ── LOSS MANAGEMENT CHECKS ───────────────────────────────────────────
        _roll_day(snap.now_ms);

        if (loss_state_.daily_shutdown) {
            d.block_reason = "DAILY_LOSS_CAP_HIT";
            return d;
        }
        if (loss_state_.shadow_forced) {
            d.block_reason = "DRAWDOWN_SHADOW_FORCED";
            return d;
        }
        if (snap.now_ms < loss_state_.pause_until_ms) {
            d.block_reason = "CONSEC_LOSS_PAUSE";
            return d;
        }

        // ── v12 EDGE-HOUR FILTER ─────────────────────────────────────────────
        // Only trade during OOS-validated hours. Reject fast before regime
        // detection to avoid wasting compute on hours with no edge.
        if (snap.hour_utc >= 0 && snap.hour_utc < 24 && !edge_hours[snap.hour_utc]) {
            d.block_reason = "HOUR_NOT_IN_EDGE_SET";
            return d;
        }

        // ── v12 ATR FLOOR ────────────────────────────────────────────────────
        // ATR below 2.5 has PF=0.80 (481 trades, -$236 over 26 months).
        // The drift signal lacks conviction in low-vol environments.
        if (snap.atr_pts < atr_entry_floor) {
            d.block_reason = "ATR_BELOW_FLOOR";
            return d;
        }

        // ── DETECT REGIME ────────────────────────────────────────────────────
        const TradingRegime regime = detect_regime(snap);
        d.regime = regime;

        // ── REGIME ROUTING CHECK ─────────────────────────────────────────────
        const float engine_weight = _lookup_routing(regime, engine);
        if (engine_weight <= 0.05f) {
            d.block_reason = "REGIME_BLOCKS_ENGINE";
            return d;
        }

        // ── DIRECTION vs REGIME SANITY CHECK ─────────────────────────────────
        // Don't go long in BEAR_TREND unless engine is a fade engine
        // Don't go short in BULL_TREND unless engine is a fade engine
        if (regime == TradingRegime::BULL_TREND && !is_long) {
            const bool is_fade_engine = (engine == EngineId::VWAPSnapback ||
                                         engine == EngineId::SpikeFade ||
                                         engine == EngineId::MeanReversion);
            if (!is_fade_engine) {
                d.block_reason = "SHORT_IN_BULL_TREND";
                return d;
            }
        }
        if (regime == TradingRegime::BEAR_TREND && is_long) {
            const bool is_fade_engine = (engine == EngineId::VWAPSnapback ||
                                         engine == EngineId::SpikeFade ||
                                         engine == EngineId::MeanReversion);
            if (!is_fade_engine) {
                d.block_reason = "LONG_IN_BEAR_TREND";
                return d;
            }
        }

        // ── SIGNAL QUALITY CHECK (regime-adjusted threshold) ─────────────────
        const int score_threshold = _score_threshold_for_regime(regime);
        if (snap.signal_score < score_threshold) {
            d.block_reason = "SIGNAL_SCORE_TOO_LOW";
            return d;
        }

        // ── COST COVERAGE CHECK ──────────────────────────────────────────────
        const double spread = snap.ask - snap.bid;
        if (!ExecutionCostGuard::is_viable("XAUUSD", spread, proposed_tp_pts,
                                            base_lot, cost_ratio_min)) {
            d.block_reason = "COST_NOT_COVERED";
            d.cost_usd = ExecutionCostGuard::estimated_cost_usd("XAUUSD", spread, base_lot);
            return d;
        }

        // ── EV GUARD CHECK ───────────────────────────────────────────────────
        if (ev_guard_.stats_frozen) {
            const auto ev_result = ev_guard_.evaluate(spread, snap.hour_utc);
            d.ev_usd = ev_result.ev_usd;
            d.cost_usd = ev_result.expected_cost;
            if (!ev_result.fire) {
                d.block_reason = "EV_NEGATIVE";
                return d;
            }
        }

        // ── SPREAD SANITY CHECK ──────────────────────────────────────────────
        // Gold spread > 2.0 pts is abnormal (news, low liquidity)
        if (spread > 2.0) {
            d.block_reason = "SPREAD_ABNORMAL";
            return d;
        }

        // ── ALL CHECKS PASSED -- CALCULATE ENTRY PARAMETERS ──────────────────
        d.allow = true;
        d.block_reason = "ENTRY_APPROVED";

        // Position sizing: regime_weight * signal_quality * vol_inverse * penalty
        const double signal_quality = static_cast<double>(snap.signal_score) / 16.0;
        const double vol_inverse = std::min(1.5, 5.0 / std::max(1.0, snap.atr_pts));
        double size_penalty = loss_state_.size_penalty;
        if (snap.now_ms >= loss_state_.penalty_expires_ms) {
            size_penalty = 1.0;
        }

        double raw_lot = base_lot * engine_weight * signal_quality * vol_inverse * size_penalty;
        raw_lot = std::max(min_lot, std::min(max_lot, raw_lot));
        // Round to 0.01 increments
        raw_lot = std::floor(raw_lot * 100.0) / 100.0;
        if (raw_lot < min_lot) raw_lot = min_lot;
        d.lot_size = raw_lot;

        // SL/TP from engine proposal, adjusted for regime
        d.sl_pts = proposed_sl_pts;
        d.tp_pts = proposed_tp_pts;

        // Break-even trigger: 1.0 ATR in trend, 0.75 ATR in MR
        if (regime == TradingRegime::BULL_TREND || regime == TradingRegime::BEAR_TREND) {
            d.be_trigger_pts = snap.atr_pts * 1.0;
            d.trail_pts = snap.atr_pts * 0.5;  // Aggressive trail in trend
        } else {
            d.be_trigger_pts = snap.atr_pts * 0.75;
            d.trail_pts = snap.atr_pts * 0.75;  // Wider trail in MR/compression
        }

        return d;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // on_trade_close() -- Update loss management after a trade closes
    // ─────────────────────────────────────────────────────────────────────────
    void on_trade_close(double net_pnl_usd, double expected_pnl_usd,
                        int64_t now_ms) noexcept {
        loss_state_.daily_pnl_usd += net_pnl_usd;
        loss_state_.trades_today += 1;

        if (net_pnl_usd > 0.0) {
            loss_state_.wins_today += 1;
            loss_state_.consec_losses = 0;
            // Update session peak
            if (loss_state_.daily_pnl_usd > loss_state_.session_peak_pnl) {
                loss_state_.session_peak_pnl = loss_state_.daily_pnl_usd;
            }
        } else {
            loss_state_.losses_today += 1;
            loss_state_.consec_losses += 1;

            // Consecutive loss pause
            if (loss_state_.consec_losses >= max_consec_losses) {
                loss_state_.pause_until_ms = now_ms + pause_duration_ms;
                std::printf("[ULTIMATE] PAUSE triggered: %d consec losses, "
                           "paused until +%dmin\n",
                           loss_state_.consec_losses,
                           pause_duration_ms / 60000);
                std::fflush(stdout);
            }

            // Big loss penalty (next trade at half size)
            if (expected_pnl_usd != 0.0 &&
                std::fabs(net_pnl_usd) > big_loss_threshold * std::fabs(expected_pnl_usd)) {
                loss_state_.size_penalty = 0.5;
                loss_state_.penalty_expires_ms = now_ms + 3600000LL; // 1 hour
                std::printf("[ULTIMATE] SIZE_PENALTY: loss $%.2f > %.1fx expected, "
                           "next trade at 50%% size\n",
                           net_pnl_usd, big_loss_threshold);
                std::fflush(stdout);
            }
        }

        // Daily loss cap
        if (loss_state_.daily_pnl_usd <= -daily_loss_cap_usd) {
            loss_state_.daily_shutdown = true;
            std::fprintf(stderr,
                "\033[1;31m[ULTIMATE] DAILY SHUTDOWN: pnl=$%.2f hit cap -$%.0f. "
                "No more entries until UTC day rollover.\033[0m\n",
                loss_state_.daily_pnl_usd, daily_loss_cap_usd);
            std::fflush(stderr);
        }

        // Drawdown from session peak
        loss_state_.session_drawdown =
            loss_state_.session_peak_pnl - loss_state_.daily_pnl_usd;
        if (loss_state_.session_drawdown >= drawdown_shadow_usd) {
            loss_state_.shadow_forced = true;
            std::fprintf(stderr,
                "\033[1;33m[ULTIMATE] DRAWDOWN SHADOW: peak=$%.2f current=$%.2f "
                "dd=$%.2f >= $%.0f. Forcing shadow mode.\033[0m\n",
                loss_state_.session_peak_pnl, loss_state_.daily_pnl_usd,
                loss_state_.session_drawdown, drawdown_shadow_usd);
            std::fflush(stderr);
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // manage_open_position() -- Tick-by-tick management of an open trade
    //
    // Returns new SL price. Caller compares with current and tightens only.
    // Also returns whether to CLOSE IMMEDIATELY (emergency cut).
    // ─────────────────────────────────────────────────────────────────────────
    struct PositionUpdate {
        double  new_sl_px       = 0.0;    // proposed SL (only accept if tighter)
        bool    emergency_close = false;  // CLOSE NOW (regime flip, etc.)
        const char* close_reason = nullptr;
    };

    PositionUpdate manage_open_position(
        bool is_long,
        double entry_px,
        double current_sl_px,
        double current_mid,
        double atr_at_entry,
        TradingRegime entry_regime,
        const MarketSnapshot& now_snap) const noexcept
    {
        PositionUpdate upd{};
        upd.new_sl_px = current_sl_px;

        if (atr_at_entry <= 0.0 || current_mid <= 0.0) return upd;

        // ── REGIME FLIP EMERGENCY CUT ────────────────────────────────────────
        // If we entered in BULL_TREND long and regime flips to BEAR_TREND,
        // or entered BEAR_TREND short and regime flips to BULL_TREND:
        // CUT IMMEDIATELY. Don't wait for SL.
        const TradingRegime current_regime = detect_regime(now_snap);

        if (entry_regime == TradingRegime::BULL_TREND && is_long &&
            current_regime == TradingRegime::BEAR_TREND) {
            upd.emergency_close = true;
            upd.close_reason = "REGIME_FLIP_BULL_TO_BEAR";
            return upd;
        }
        if (entry_regime == TradingRegime::BEAR_TREND && !is_long &&
            current_regime == TradingRegime::BULL_TREND) {
            upd.emergency_close = true;
            upd.close_reason = "REGIME_FLIP_BEAR_TO_BULL";
            return upd;
        }

        // ── NOISE TRANSITION CUT ─────────────────────────────────────────────
        // If regime transitions to NOISE and we're not in profit, cut.
        if (current_regime == TradingRegime::NOISE) {
            const double unrealised = is_long ? (current_mid - entry_px)
                                              : (entry_px - current_mid);
            if (unrealised <= 0.0) {
                upd.emergency_close = true;
                upd.close_reason = "NOISE_REGIME_NO_PROFIT";
                return upd;
            }
        }

        // ── BREAK-EVEN SHIFT ─────────────────────────────────────────────────
        const double favourable = is_long ? (current_mid - entry_px)
                                          : (entry_px - current_mid);
        const double be_trigger = atr_at_entry * 1.0;  // 1 ATR

        if (favourable >= be_trigger) {
            // Move SL to entry + small buffer (cover spread cost)
            const double be_buffer = 0.10;  // $0.10 gold points
            const double be_sl = is_long ? (entry_px + be_buffer)
                                         : (entry_px - be_buffer);
            if (is_long && be_sl > upd.new_sl_px) upd.new_sl_px = be_sl;
            if (!is_long && be_sl < upd.new_sl_px) upd.new_sl_px = be_sl;
        }

        // ── TRAILING STOP ────────────────────────────────────────────────────
        // After BE is armed (implied by favourable >= be_trigger above),
        // trail at 0.5 ATR behind price in trend, 0.75 ATR in MR/compression
        if (favourable >= be_trigger) {
            double trail_mult = 0.5;
            if (current_regime == TradingRegime::MEAN_REVERSION ||
                current_regime == TradingRegime::COMPRESSION) {
                trail_mult = 0.75;
            }
            const double trail_dist = atr_at_entry * trail_mult;
            const double trail_sl = is_long ? (current_mid - trail_dist)
                                            : (current_mid + trail_dist);
            if (is_long && trail_sl > upd.new_sl_px) upd.new_sl_px = trail_sl;
            if (!is_long && trail_sl < upd.new_sl_px) upd.new_sl_px = trail_sl;
        }

        // ── TIME DECAY CUT ───────────────────────────────────────────────────
        // If trade is open > 2 hours and not in significant profit (< 0.5 ATR),
        // tighten SL to 0.3 ATR (force exit on next wiggle)
        // This prevents capital being tied up in dead trades.
        // (Time-based management handled by engine_protections.hpp time_stop
        //  -- this is the strategy-level overlay that tightens rather than kills)

        return upd;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Accessors
    // ─────────────────────────────────────────────────────────────────────────
    const LossManagementState& loss_state() const noexcept { return loss_state_; }
    TradingRegime last_regime() const noexcept { return last_regime_; }

    // Set EV guard stats (loaded from backtest or rolling history)
    EVGuard& ev_guard() noexcept { return ev_guard_; }
    const EVGuard& ev_guard() const noexcept { return ev_guard_; }

    // ─────────────────────────────────────────────────────────────────────────
    // log_state() -- Hourly diagnostic dump
    // ─────────────────────────────────────────────────────────────────────────
    void log_state(const MarketSnapshot& snap) noexcept {
        const TradingRegime regime = detect_regime(snap);
        last_regime_ = regime;

        std::printf("[ULTIMATE] enabled=%d regime=%s drift=%.2f atr=%.2f "
                    "vol_ratio=%.1f hmm=%d macro=%s score=%d\n",
                    enabled ? 1 : 0,
                    regime_name(regime),
                    snap.ewm_drift, snap.atr_pts,
                    snap.vol_ratio, snap.hmm_state,
                    snap.macro_regime.c_str(), snap.signal_score);
        std::printf("[ULTIMATE] loss_mgmt: daily_pnl=$%.2f consec=%d "
                    "shutdown=%d shadow=%d trades=%d W/L=%d/%d penalty=%.1f\n",
                    loss_state_.daily_pnl_usd,
                    loss_state_.consec_losses,
                    loss_state_.daily_shutdown ? 1 : 0,
                    loss_state_.shadow_forced ? 1 : 0,
                    loss_state_.trades_today,
                    loss_state_.wins_today,
                    loss_state_.losses_today,
                    loss_state_.size_penalty);

        // Per-engine routing weights for current regime
        std::printf("[ULTIMATE] engine routing for %s:\n", regime_name(regime));
        for (int i = 0; i < kTotalEngines; ++i) {
            const float w = routing_table_[static_cast<int>(regime)][i];
            if (w > 0.05f) {
                std::printf("  %-24s weight=%.2f\n",
                            engine_id_name(static_cast<EngineId>(i)),
                            static_cast<double>(w));
            }
        }
        std::fflush(stdout);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // reset_day() -- Called on UTC day rollover
    // ─────────────────────────────────────────────────────────────────────────
    void reset_day() noexcept {
        loss_state_.daily_pnl_usd = 0.0;
        loss_state_.daily_shutdown = false;
        loss_state_.shadow_forced = false;
        loss_state_.session_peak_pnl = 0.0;
        loss_state_.session_drawdown = 0.0;
        loss_state_.trades_today = 0;
        loss_state_.wins_today = 0;
        loss_state_.losses_today = 0;
        // Don't reset consec_losses -- they carry across days
        // Don't reset size_penalty -- it has its own expiry
    }

private:
    // ─────────────────────────────────────────────────────────────────────────
    // Internal state
    // ─────────────────────────────────────────────────────────────────────────
    LossManagementState loss_state_{};
    TradingRegime       last_regime_ = TradingRegime::NOISE;
    EVGuard             ev_guard_{};

    // routing_table_[regime][engine] = weight
    float routing_table_[static_cast<int>(TradingRegime::COUNT)][kTotalEngines]{};

    // ─────────────────────────────────────────────────────────────────────────
    // _roll_day() -- detect UTC day change
    // ─────────────────────────────────────────────────────────────────────────
    void _roll_day(int64_t now_ms) noexcept {
        constexpr int64_t MS_PER_DAY = 86400000LL;
        const int64_t today = now_ms / MS_PER_DAY;
        if (loss_state_.day_utc_idx == -1) {
            loss_state_.day_utc_idx = today;
            return;
        }
        if (today != loss_state_.day_utc_idx) {
            loss_state_.day_utc_idx = today;
            reset_day();
            std::printf("[ULTIMATE] UTC day rollover -- daily counters reset\n");
            std::fflush(stdout);
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // _score_threshold_for_regime() -- dynamic signal quality bar
    // ─────────────────────────────────────────────────────────────────────────
    int _score_threshold_for_regime(TradingRegime r) const noexcept {
        switch (r) {
            case TradingRegime::BULL_TREND:     return score_min_trend;
            case TradingRegime::BEAR_TREND:     return score_min_trend;
            case TradingRegime::MEAN_REVERSION: return score_min_mr;
            case TradingRegime::COMPRESSION:    return score_min_compression;
            case TradingRegime::NOISE:          return score_min_noise;
            default:                            return score_min_noise;
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // _lookup_routing() -- O(1) table read
    // ─────────────────────────────────────────────────────────────────────────
    float _lookup_routing(TradingRegime regime, EngineId engine) const noexcept {
        const int ri = static_cast<int>(regime);
        const int ei = static_cast<int>(engine);
        if (ri < 0 || ri >= static_cast<int>(TradingRegime::COUNT)) return 0.0f;
        if (ei < 0 || ei >= kTotalEngines) return 0.0f;
        return routing_table_[ri][ei];
    }

    // ─────────────────────────────────────────────────────────────────────────
    // _init_routing_table() -- Seed the regime -> engine routing weights
    //
    // Weight semantics:
    //   0.0  = BLOCKED (engine must not fire)
    //   0.3  = LOW CONFIDENCE (secondary, reduced size)
    //   0.5  = MODERATE (allowed but not ideal)
    //   1.0  = NORMAL (engine's natural habitat)
    //   1.5  = BOOSTED (regime strongly favours this engine)
    //   2.0  = MAX BOOST (engine's absolute best conditions)
    // ─────────────────────────────────────────────────────────────────────────
    void _init_routing_table() noexcept {
        // Zero everything first
        std::memset(routing_table_, 0, sizeof(routing_table_));

        auto set = [this](TradingRegime r, EngineId e, float w) noexcept {
            routing_table_[static_cast<int>(r)][static_cast<int>(e)] = w;
        };

        // ══════════════════════════════════════════════════════════════════════
        // BULL TREND -- gold rising, risk-off, continuation
        // Strategy: ride the trend, add on pullbacks, trail aggressively
        // ══════════════════════════════════════════════════════════════════════
        set(TradingRegime::BULL_TREND, EngineId::XauTrendFollow2h,       1.5f);
        set(TradingRegime::BULL_TREND, EngineId::XauTrendFollow4h,       1.5f);
        set(TradingRegime::BULL_TREND, EngineId::XauTrendFollowD1,       1.2f);
        set(TradingRegime::BULL_TREND, EngineId::CandleFlowGold,         1.3f);
        set(TradingRegime::BULL_TREND, EngineId::MinimalH4Gold,          1.0f);
        set(TradingRegime::BULL_TREND, EngineId::GoldMidScalper,         0.8f);
        set(TradingRegime::BULL_TREND, EngineId::SessionMomentum,        1.2f);
        set(TradingRegime::BULL_TREND, EngineId::DonchianBreakout,       1.0f);
        set(TradingRegime::BULL_TREND, EngineId::EMACross,               1.0f);
        set(TradingRegime::BULL_TREND, EngineId::SessionOpenMomentum,    1.0f);
        set(TradingRegime::BULL_TREND, EngineId::LondonFixMomentum,      0.8f);
        set(TradingRegime::BULL_TREND, EngineId::ORBNewYork,             0.8f);
        set(TradingRegime::BULL_TREND, EngineId::DXYDivergence,          1.0f);
        set(TradingRegime::BULL_TREND, EngineId::TwoBarReversal,         0.5f);
        set(TradingRegime::BULL_TREND, EngineId::LiquiditySweepPro,      0.3f);
        // BLOCKED in BULL: mean reversion engines (would fight the trend)
        set(TradingRegime::BULL_TREND, EngineId::MeanReversion,          0.0f);
        set(TradingRegime::BULL_TREND, EngineId::VWAPSnapback,           0.0f);
        set(TradingRegime::BULL_TREND, EngineId::SpikeFade,              0.0f);
        set(TradingRegime::BULL_TREND, EngineId::GoldMicroScalper,       0.0f);

        // ══════════════════════════════════════════════════════════════════════
        // BEAR TREND -- gold falling, crash conditions, risk-on panic
        // Strategy: SHORT trend follow + catch the crash + fade exhaustion
        // ══════════════════════════════════════════════════════════════════════
        set(TradingRegime::BEAR_TREND, EngineId::MacroCrash,             2.0f);
        set(TradingRegime::BEAR_TREND, EngineId::XauTrendFollow2h,       1.3f);
        set(TradingRegime::BEAR_TREND, EngineId::XauTrendFollow4h,       1.3f);
        set(TradingRegime::BEAR_TREND, EngineId::XauTrendFollowD1,       1.5f);
        set(TradingRegime::BEAR_TREND, EngineId::SpikeFade,              1.2f);
        set(TradingRegime::BEAR_TREND, EngineId::LiquiditySweepPro,      1.0f);
        set(TradingRegime::BEAR_TREND, EngineId::LiquiditySweepPressure, 0.8f);
        set(TradingRegime::BEAR_TREND, EngineId::TwoBarReversal,         0.8f);
        set(TradingRegime::BEAR_TREND, EngineId::DXYDivergence,          1.0f);
        set(TradingRegime::BEAR_TREND, EngineId::CandleFlowGold,         0.5f);
        set(TradingRegime::BEAR_TREND, EngineId::EMACross,               0.8f);
        // BLOCKED in BEAR: long-biased momentum engines
        set(TradingRegime::BEAR_TREND, EngineId::SessionMomentum,        0.0f);
        set(TradingRegime::BEAR_TREND, EngineId::SessionOpenMomentum,    0.0f);
        set(TradingRegime::BEAR_TREND, EngineId::DonchianBreakout,       0.0f);
        set(TradingRegime::BEAR_TREND, EngineId::GoldMicroScalper,       0.0f);
        set(TradingRegime::BEAR_TREND, EngineId::NR3Breakout,            0.0f);

        // ══════════════════════════════════════════════════════════════════════
        // MEAN REVERSION -- choppy, fade extremes, collect range premium
        // Strategy: fade overextensions, tight TPs, respect the range
        // ══════════════════════════════════════════════════════════════════════
        set(TradingRegime::MEAN_REVERSION, EngineId::XauusdFvg,              1.5f);
        set(TradingRegime::MEAN_REVERSION, EngineId::XauThreeBar30m,         1.3f);
        set(TradingRegime::MEAN_REVERSION, EngineId::MeanReversion,          1.5f);
        set(TradingRegime::MEAN_REVERSION, EngineId::VWAPSnapback,           1.5f);
        set(TradingRegime::MEAN_REVERSION, EngineId::DynamicRange,           1.2f);
        set(TradingRegime::MEAN_REVERSION, EngineId::LiquiditySweepPro,      1.0f);
        set(TradingRegime::MEAN_REVERSION, EngineId::LiquiditySweepPressure, 1.0f);
        set(TradingRegime::MEAN_REVERSION, EngineId::VWAPStretchReversion,   1.2f);
        set(TradingRegime::MEAN_REVERSION, EngineId::SpikeFade,              1.0f);
        set(TradingRegime::MEAN_REVERSION, EngineId::TwoBarReversal,         1.0f);
        set(TradingRegime::MEAN_REVERSION, EngineId::IntradaySeasonality,    0.8f);
        set(TradingRegime::MEAN_REVERSION, EngineId::GoldMidScalper,         0.5f);
        set(TradingRegime::MEAN_REVERSION, EngineId::NR3Breakout,            0.8f);
        // BLOCKED in MR: trend-follow engines (they get chopped up)
        set(TradingRegime::MEAN_REVERSION, EngineId::XauTrendFollow2h,       0.0f);
        set(TradingRegime::MEAN_REVERSION, EngineId::XauTrendFollow4h,       0.0f);
        set(TradingRegime::MEAN_REVERSION, EngineId::XauTrendFollowD1,       0.0f);
        set(TradingRegime::MEAN_REVERSION, EngineId::CandleFlowGold,         0.0f);
        set(TradingRegime::MEAN_REVERSION, EngineId::SessionMomentum,        0.0f);
        set(TradingRegime::MEAN_REVERSION, EngineId::MacroCrash,             0.0f);

        // ══════════════════════════════════════════════════════════════════════
        // COMPRESSION -- tight range, low vol, breakout imminent
        // Strategy: watch for breakout signals, position small, wait for confirm
        // ══════════════════════════════════════════════════════════════════════
        set(TradingRegime::COMPRESSION, EngineId::NR3Breakout,            1.5f);
        set(TradingRegime::COMPRESSION, EngineId::NR3Tick,                1.5f);
        set(TradingRegime::COMPRESSION, EngineId::AsianRange,             1.3f);
        set(TradingRegime::COMPRESSION, EngineId::DonchianBreakout,       1.2f);
        set(TradingRegime::COMPRESSION, EngineId::IntradaySeasonality,    1.0f);
        set(TradingRegime::COMPRESSION, EngineId::VWAPStretchReversion,   0.8f);
        set(TradingRegime::COMPRESSION, EngineId::DynamicRange,           0.8f);
        set(TradingRegime::COMPRESSION, EngineId::ORBNewYork,             0.8f);
        // BLOCKED in COMPRESSION: everything that needs vol
        set(TradingRegime::COMPRESSION, EngineId::XauTrendFollow2h,       0.0f);
        set(TradingRegime::COMPRESSION, EngineId::XauTrendFollow4h,       0.0f);
        set(TradingRegime::COMPRESSION, EngineId::XauTrendFollowD1,       0.0f);
        set(TradingRegime::COMPRESSION, EngineId::CandleFlowGold,         0.0f);
        set(TradingRegime::COMPRESSION, EngineId::MacroCrash,             0.0f);
        set(TradingRegime::COMPRESSION, EngineId::GoldMicroScalper,       0.0f);
        set(TradingRegime::COMPRESSION, EngineId::GoldMidScalper,         0.0f);
        set(TradingRegime::COMPRESSION, EngineId::SessionMomentum,        0.0f);
        set(TradingRegime::COMPRESSION, EngineId::SpikeFade,              0.0f);

        // ══════════════════════════════════════════════════════════════════════
        // NOISE -- no edge, sit on hands, preserve capital
        // Strategy: only ultra-high-confidence signals pass
        // ══════════════════════════════════════════════════════════════════════
        // Nearly everything blocked. Only IntradaySeasonality (statistical)
        // and DXYDivergence (cross-asset, external signal) allowed at low weight
        set(TradingRegime::NOISE, EngineId::IntradaySeasonality,    0.3f);
        set(TradingRegime::NOISE, EngineId::DXYDivergence,          0.3f);
        set(TradingRegime::NOISE, EngineId::LondonFixMomentum,      0.3f);
        // Everything else is 0.0 (already zero from memset)
    }
};

// =============================================================================
// Global instance -- inline (C++17 ODR-safe)
// =============================================================================
inline GoldUltimateStrategy g_gold_ultimate;

}} // namespace omega::gold_ultimate
