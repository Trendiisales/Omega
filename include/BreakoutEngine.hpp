#pragma once
// ==============================================================================
// BreakoutEngine — CRTP policy-based compression breakout engine
// One instance per primary symbol (MES, MNQ, MCL).
// CRTP eliminates all virtual dispatch — hot path is fully inlined.
// ==============================================================================
#include <deque>
#include <array>
#include <string>
#include <chrono>
#include <cmath>
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <functional>
#include <cstring>
#include "OmegaTradeLedger.hpp"

namespace omega {

enum class Phase : uint8_t { FLAT = 0, COMPRESSION = 1, BREAKOUT_WATCH = 2, IN_TRADE = 3 };

struct BreakoutSignal
{
    bool        valid            = false;
    bool        is_long          = true;
    double      entry            = 0.0;
    double      tp               = 0.0;
    double      sl               = 0.0;
    const char* reason           = "";
    // Edge model outputs — available for ranking
    double      net_edge         = 0.0;
    double      breakout_strength= 0.0;
    double      momentum_score   = 0.0;
    double      vol_score        = 0.0;
};

struct OpenPos
{
    bool    active          = false;
    bool    is_long         = true;
    double  entry           = 0.0;
    double  tp              = 0.0;
    double  sl              = 0.0;
    double  size            = 1.0;
    double  mfe             = 0.0;
    double  mae             = 0.0;
    double  sl_pct          = 0.0;  // SL% used at entry — drives trail arm thresholds
    int64_t entry_ts        = 0;
    double  spread_at_entry = 0.0;
    char    regime[32]      = {};
    // Pyramid tracking — one add-on per trade in expansion regime
    bool    pyramid_armed   = false;
    bool    pyramid_done    = false;
    double  pyramid_entry   = 0.0;
    double  pyramid_tp      = 0.0;
    double  pyramid_sl      = 0.0;
    // Regime at entry — for regime-flip exit
    char    entry_regime[32] = {};   // supervisor regime name at entry time
};

// ==============================================================================
// Edge model — signal strength, cost model, exhaustion filter, adaptive TP/SL
// ==============================================================================

struct EdgeConfig {
    double cost_spread_mult  = 0.3;   // cost = spread * this
    double min_range_factor  = 0.12;  // net_move must exceed comp_range * this
    double min_edge_bp       = 6.0;
    double exhaustion_mult   = 3.0;
    double min_edge_buffer   = 0.0;
};

struct EdgeResult {
    double expected_move    = 0.0;
    double total_cost       = 0.0;
    double net_edge         = 0.0;
    double tp_price         = 0.0;
    double sl_price         = 0.0;
    double size             = 0.0;
    double breakout_strength= 0.0;
    double momentum_score   = 0.0;
    double vol_score        = 0.0;
    bool   valid            = false;
};

inline EdgeResult compute_edge_and_execution(
    double mid, double spread,
    double breakout_move,
    double comp_high, double comp_low,
    double recent_vol, double momentum,
    bool is_long,
    double account_equity,
    const EdgeConfig& cfg) noexcept
{
    EdgeResult r{};
    const double comp_range = comp_high - comp_low;
    if (mid <= 0.0 || comp_range <= 0.0) return r;
    // breakout_move is measured from comp edge (mid - comp_high for long).
    // With spread*0.5 tolerance, this is always >= spread*0.5 when we get here.
    if (breakout_move <= 0.0) return r;

    // ── Exhaustion filter ─────────────────────────────────────────────────────
    if (breakout_move > comp_range * cfg.exhaustion_mult) return r;

    // ── Cost model ───────────────────────────────────────────────────────────
    // cost = spread * 0.5 (half-spread — mid-based move already net of half spread)
    const double cost      = spread * cfg.cost_spread_mult;
    const double net_move  = breakout_move - cost;

    // ── Edge in basis points ──────────────────────────────────────────────────
    const double edge_bp   = (mid > 0.0) ? (net_move / mid * 10000.0) : 0.0;

    // ── Validation ───────────────────────────────────────────────────────────
    // Both conditions must pass:
    //   1. net_move > comp_range * min_range_factor  (meaningful relative to structure)
    //   2. edge_bp  >= min_edge_bp                   (meaningful relative to price)
    const bool edge_ok = (net_move > comp_range * cfg.min_range_factor) &&
                         (edge_bp  >= cfg.min_edge_bp);
    if (!edge_ok) return r;

    // ── TP / SL ───────────────────────────────────────────────────────────────
    // TP raised from comp_range×0.8 to comp_range×1.6 (4:1 R:R vs prior 2:1).
    // Rationale: with TP=0.8×range, trail2 arm (2×SL=0.8×range) = TP exactly.
    // Trail2 never fires — trade exits at fixed TP, capping every genuine run.
    // At 1.6×range: trail2 fires at 0.8×range, then trails the remaining move.
    // This matches Gold's engine R:R profile (TP=80ticks, SL=30ticks = 2.67:1).
    const double tp_dist = comp_range * 1.6;
    const double sl_dist = std::max(comp_range * 0.4, spread * 1.5);

    // ── R:R validity gate ─────────────────────────────────────────────────────
    // Reject if sl_dist >= tp_dist (R:R < 1:1 before spread cost).
    // Fires when spread dominates comp_range×0.4 — wide-spread Asia session.
    // Example: USTEC comp_range=2pts spread=3.24pts → sl=4.86 tp=1.6 → R:R=0.33
    // A trade with sl >= tp has negative expectancy before costs. Block it.
    if (sl_dist >= tp_dist) return r;

    r.tp_price = is_long ? mid + tp_dist : mid - tp_dist;
    r.sl_price = is_long ? mid - sl_dist : mid + sl_dist;

    // ── Position sizing ───────────────────────────────────────────────────────
    const double risk_per_trade = account_equity * 0.002;
    r.size = (sl_dist > 0.0) ? (risk_per_trade / sl_dist) : 0.01;
    if (r.size < 0.01) r.size = 0.01;
    if (r.size > 5.0)  r.size = 5.0;

    r.expected_move     = net_move;
    r.total_cost        = cost;
    r.net_edge          = net_move;
    r.breakout_strength = (comp_range > 0.0) ? breakout_move / comp_range : 0.0;
    r.momentum_score    = std::fabs(momentum);
    r.vol_score         = recent_vol;
    r.valid             = true;
    return r;
}

// ==============================================================================
// Trade ranking + selection — scores and filters candidates, picks best setup
// ==============================================================================

struct RankingConfig {
    int    max_trades_per_cycle  = 1;
    double min_score_threshold   = 0.2;
    double score_edge_weight     = 0.5;
    double score_momentum_weight = 0.2;
    double score_vol_weight      = 0.2;
    double score_breakout_weight = 0.1;
};

struct TradeCandidate {
    bool        valid            = false;
    bool        is_long          = true;
    double      entry            = 0.0;
    double      tp               = 0.0;
    double      sl               = 0.0;
    double      size             = 0.0;
    double      expected_move    = 0.0;
    double      cost             = 0.0;
    double      net_edge         = 0.0;
    double      breakout_strength= 0.0;
    double      momentum         = 0.0;
    double      vol              = 0.0;
    double      score            = 0.0;
    const char* symbol           = "";
};

inline TradeCandidate build_candidate(
    const EdgeResult& e, bool is_long, double entry, const char* sym) noexcept
{
    TradeCandidate t{};
    if (!e.valid) return t;
    t.valid             = true;
    t.is_long           = is_long;
    t.entry             = entry;
    t.tp                = e.tp_price;
    t.sl                = e.sl_price;
    t.size              = e.size;
    t.expected_move     = e.expected_move;
    t.cost              = e.total_cost;
    t.net_edge          = e.net_edge;
    t.breakout_strength = e.breakout_strength;
    t.momentum          = e.momentum_score;
    t.vol               = e.vol_score;
    t.symbol            = sym ? sym : "";
    return t;
}

inline double compute_trade_score(
    const TradeCandidate& t, const RankingConfig& cfg) noexcept
{
    if (!t.valid) return -1.0;
    // Normalize net_edge by entry price — removes price-scale bias across symbols.
    // Gold net_edge=2.0 at price=4850 = 0.041% vs EURUSD net_edge=0.0002 at 1.08 = 0.019%.
    // Without normalization gold always dominates ranking regardless of setup quality.
    const double norm_edge = (t.entry > 0.0) ? (t.net_edge / t.entry) : 0.0;
    return  (norm_edge             * cfg.score_edge_weight)
          + (std::fabs(t.momentum) * cfg.score_momentum_weight)
          + (t.vol                 * cfg.score_vol_weight)
          + (t.breakout_strength   * cfg.score_breakout_weight);
}

inline std::vector<TradeCandidate> select_best_trades(
    std::vector<TradeCandidate>& candidates,
    const RankingConfig& cfg) noexcept
{
    for (auto& t : candidates)
        t.score = compute_trade_score(t, cfg);

    // Remove only invalid candidates — do NOT gate on min_score_threshold.
    // Ranking selects the best when multiple signals compete; it must never
    // block a single valid signal that already passed the edge model.
    candidates.erase(
        std::remove_if(candidates.begin(), candidates.end(),
            [](const TradeCandidate& t) { return !t.valid; }),
        candidates.end());

    std::sort(candidates.begin(), candidates.end(),
        [](const TradeCandidate& a, const TradeCandidate& b) {
            return a.score > b.score;
        });

    if (static_cast<int>(candidates.size()) > cfg.max_trades_per_cycle)
        candidates.resize(cfg.max_trades_per_cycle);

    return candidates;
}

// ==============================================================================
// CRTP base
// Derived must be: class MyEngine : public BreakoutEngineBase<MyEngine>
// Optional overrides:
//   bool shouldTrade(double bid, double ask, double spread_pct, double lat_ms)
//   void onSignal(const BreakoutSignal&)
// ==============================================================================
template<typename Derived>
class BreakoutEngineBase
{
public:
    // ── Config — set before first tick ───────────────────────────────────────
    double      VOL_THRESH_PCT        = 0.050;
    double      TP_PCT                = 0.400;
    double      SL_PCT                = 2.000;
    int         COMPRESSION_LOOKBACK  = 50;
    int         BASELINE_LOOKBACK     = 200;
    double      COMPRESSION_THRESHOLD = 0.80;  // enter compression when ratio < this
    double      COMP_EXIT_THRESHOLD   = 0.95;  // hysteresis: stay compressed while ratio < this, exit when >= this
    int         MAX_HOLD_SEC          = 1500;
    int         MIN_GAP_SEC           = 180;
    double      MAX_SPREAD_PCT        = 0.05;
    double      MOMENTUM_THRESH_PCT   = 0.05;
    double      MIN_BREAKOUT_PCT      = 0.25;
    double      MIN_COMP_RANGE        = 0.0;
    double      ACCOUNT_EQUITY        = 10000.0;
    EdgeConfig  EDGE_CFG;
    int         MAX_TRADES_PER_MIN    = 2;
    double      ENTRY_SIZE            = 0.01;
    bool        AGGRESSIVE_SHADOW     = false;
    const char* symbol                = "???";
    int         WATCH_TIMEOUT_SEC     = 300;
    // ── Compression stability params (spec values) ────────────────────────────
    // range_tolerance: compression range may expand up to this multiple of the
    // initial range before it is considered a genuine breakout of structure.
    // 1.25 = allow 25% range expansion before declaring structure broken.
    double      COMP_RANGE_TOLERANCE  = 1.25;
    // drift_tolerance: price may drift up to this fraction of comp range inside
    // structure without triggering a reset.
    // 0.35 = 35% of range = ~2pts on ESTX50 with 6pt range.
    double      COMP_DRIFT_TOLERANCE  = 0.35;
    // violation_ticks: require this many consecutive ticks breaching ALL three
    // reset conditions before actually resetting. 3 = per spec.
    int         COMP_VIOLATION_TICKS  = 3;
    // min_life: ignore violations for this many ticks after entering compression.
    // Prevents immediate churn on entry. 12 = per spec.
    int         COMP_MIN_LIFE_TICKS   = 12;
    // re-entry cooldown: ticks to wait in FLAT after a failed arm before
    // allowing next compression detection. Stops COMPRESSION→fail churn.
    int         COMP_REENTRY_DELAY    = 5;
    // confirm_ticks: price must stay OUTSIDE the compression boundary for this
    // many consecutive ticks before the breakout signal fires.
    // Mirrors MIN_BREAK_TICKS in BracketEngine — prevents single-tick sweep
    // spikes (e.g. London open liquidity sweeps) from triggering a real entry.
    // The counter resets to 0 if price pulls back inside the range on any tick.
    // 0 = disabled (legacy behaviour, fires on first qualifying tick).
    // Recommended: 3 for gold/silver, 2 for FX/indices.
    int         MIN_CONFIRM_TICKS     = 0;

    // ── Observable state (read by telemetry thread) ───────────────────────────
    Phase   phase          = Phase::FLAT;
    double  comp_high      = 0.0;
    double  comp_low       = 0.0;
    int     watch_ticks    = 0;   // kept for GUI telemetry display only
    double  recent_vol_pct = 0.0;
    double  base_vol_pct   = 0.0;
    int     signal_count   = 0;
    OpenPos pos;

    using CloseCallback = std::function<void(const TradeRecord&)>;

private:
    // ── Momentum window (20 ticks) ────────────────────────────────────────────
    std::deque<double> m_momentum_window;   // last 20 mids for momentum gate
    static constexpr int MOMENTUM_WINDOW = 20;

    // ── Range window (50 ticks) for structural break gate ────────────────────
    std::deque<double> m_range_window;      // last 50 mids for hi/lo range
    static constexpr int RANGE_WINDOW = 50;

    // ── Rate limiter ──────────────────────────────────────────────────────────
    std::deque<int64_t> m_trade_times;      // timestamps of recent entries

    // ── Tick direction counter ────────────────────────────────────────────────
    // +N = N consecutive upticks, -N = N consecutive downticks. Reset on flip.
    // Breakout with 2+ ticks in direction = real order flow, not a spike.
    int    m_tick_run  = 0;
    double m_last_mid  = 0.0;

    // ── Watch phase timing ────────────────────────────────────────────────────
    int64_t m_watch_start_ts = 0;  // unix seconds when BREAKOUT_WATCH started

public:

    // ── Telemetry accessors ───────────────────────────────────────────────────
    // Returns unix-second timestamp until which SL cooldown is active (0 = not cooling)
    int64_t sl_cooldown_until() const noexcept { return m_sl_cooldown_until; }
    // Returns unix-second timestamp until which the given side is chop-paused (0 = not paused)
    // side: 0=LONG, 1=SHORT
    int64_t side_pause_until(int side) const noexcept {
        return (side == 0 || side == 1) ? m_side_pause_until[static_cast<size_t>(side)] : 0;
    }

    // ── Default CRTP hooks ────────────────────────────────────────────────────
    bool shouldTrade(double /*bid*/, double /*ask*/,
                     double spread_pct, double latency_ms) const noexcept
    {
        return spread_pct <= MAX_SPREAD_PCT;
    }
    void onSignal(const BreakoutSignal& /*sig*/) const noexcept {}

    // ── update() — call on every tick ────────────────────────────────────────
    // can_enter=true  → full processing including new entries
    // can_enter=false → warmup + position management only, no new entries
    [[nodiscard]] BreakoutSignal update(double bid, double ask,
                                        double latency_ms,
                                        const char* macro_regime,
                                        CloseCallback on_close,
                                        bool can_enter = true) noexcept
    {
        if (bid <= 0.0 || ask <= 0.0) return {};

        const double mid        = (bid + ask) * 0.5;
        const double spread     = ask - bid;
        const double spread_pct = (mid > 0.0) ? (spread / mid * 100.0) : 999.0;

        m_prices.push_back(mid);
        if (static_cast<int>(m_prices.size()) > BASELINE_LOOKBACK * 2)
            m_prices.pop_front();

        // ── VWAP update (daily reset at UTC midnight) ─────────────────────
        {
            const auto t_v = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
            struct tm ti_v{};
#ifdef _WIN32
            gmtime_s(&ti_v, &t_v);
#else
            gmtime_r(&t_v, &ti_v);
#endif
            if (ti_v.tm_yday != m_vwap_last_day) {
                m_vwap_cum_pv  = 0.0;
                m_vwap_cum_vol = 0.0;
                m_vwap         = 0.0;
                m_vwap_last_day = ti_v.tm_yday;
            }
            m_vwap_cum_pv  += mid;
            m_vwap_cum_vol += 1.0;
            m_vwap = (m_vwap_cum_vol > 0.0) ? (m_vwap_cum_pv / m_vwap_cum_vol) : mid;
        }

        // Maintain momentum window (20 ticks)
        m_momentum_window.push_back(mid);
        if (static_cast<int>(m_momentum_window.size()) > MOMENTUM_WINDOW)
            m_momentum_window.pop_front();

        // Maintain range window (50 ticks)
        m_range_window.push_back(mid);
        if (static_cast<int>(m_range_window.size()) > RANGE_WINDOW)
            m_range_window.pop_front();

        // Update tick direction run counter
        if (m_last_mid > 0.0) {
            if (mid > m_last_mid)       m_tick_run = (m_tick_run > 0) ? m_tick_run + 1 : 1;
            else if (mid < m_last_mid)  m_tick_run = (m_tick_run < 0) ? m_tick_run - 1 : -1;
            // equal tick: keep current run
        }
        m_last_mid = mid;

        if (static_cast<int>(m_prices.size()) < COMPRESSION_LOOKBACK + 1) return {};

        // Compute volatilities
        // During warmup (< BASELINE_LOOKBACK ticks): use the longest window we have
        // so compression detection is meaningful from the start.
        // Without this, base_vol = recent_vol → in_compression always false → FLAT forever.
        recent_vol_pct = rangePct(m_prices.cend() - COMPRESSION_LOOKBACK, m_prices.cend());
        if (static_cast<int>(m_prices.size()) >= BASELINE_LOOKBACK) {
            base_vol_pct = rangePct(m_prices.cend() - BASELINE_LOOKBACK, m_prices.cend());
        } else {
            // Warmup: use all available ticks as baseline.
            // This lets compression detection work from tick COMPRESSION_LOOKBACK+1 onwards.
            base_vol_pct = rangePct(m_prices.cbegin(), m_prices.cend());
        }

        // ── Manage open position ──────────────────────────────────────────────
        if (pos.active) {
            const double move = pos.is_long ? (mid - pos.entry) : (pos.entry - mid);
            if (move  > pos.mfe) pos.mfe =  move;
            if (-move > pos.mae) pos.mae = -move;

            // ── REGIME-FLIP EXIT (Gold pattern) ──────────────────────────────
            // If supervisor regime changes after REGIME_FLIP_MIN_HOLD_SEC,
            // the structure that justified entry has changed — exit at mid.
            // Cap at SL if price has blown past it (reconnect/sparse tick safety).
            if (macro_regime && pos.entry_regime[0] != '\0' &&
                std::strncmp(macro_regime, pos.entry_regime, 31) != 0 &&
                (nowSec() - pos.entry_ts) >= REGIME_FLIP_MIN_HOLD_SEC) {
                const bool sl_breached = pos.is_long ? (mid < pos.sl) : (mid > pos.sl);
                const double flip_exit = sl_breached ? pos.sl : mid;
                std::cout << "[ENG-" << symbol << "] REGIME_FLIP exit="
                          << flip_exit << " was=" << pos.entry_regime
                          << " now=" << macro_regime << "\n";
                std::cout.flush();
                closePos(flip_exit, "REGIME_FLIP", latency_ms, macro_regime, on_close);
                return {};
            }

            // TP/SL checks use the aggressive fill side of the spread.
            // Long exits sell at bid; short exits buy back at ask.
            // Using mid was understating slippage on SL hits.
            if ( pos.is_long && bid >= pos.tp) { closePos(pos.tp,  "TP_HIT",  latency_ms, macro_regime, on_close); return {}; }
            if (!pos.is_long && ask <= pos.tp) { closePos(pos.tp,  "TP_HIT",  latency_ms, macro_regime, on_close); return {}; }
            if ( pos.is_long && bid <= pos.sl) { closePos(pos.sl,  "SL_HIT",  latency_ms, macro_regime, on_close); return {}; }
            if (!pos.is_long && ask >= pos.sl) { closePos(pos.sl,  "SL_HIT",  latency_ms, macro_regime, on_close); return {}; }

            // ── TRAILING STOP ─────────────────────────────────────────────────
            // Arms and distances are SL-relative, not fixed %.
            // Fixed % arms were dead code for most instruments:
            //   SP500/GER30/UK100/EURUSD: lock arm (0.60%) >= TP → trail never fired
            //   NQ/DJ30/NAS100/Silver:    trail1 arm (1.00%) >= TP → only lock fired
            //
            // NEW: all thresholds derived from pos.sl_pct (SL% used at entry).
            //   Lock  arm  = 0.50 × SL_PCT  → arms at 50% of risk taken
            //   Trail1 arm = 1.00 × SL_PCT  → arms once we're 1× SL in profit
            //   Trail2 arm = 2.00 × SL_PCT  → tight trail once 2× SL in profit
            //   Trail1 dist= 0.40 × SL_PCT  → trail 40% of SL behind mid
            //   Trail2 dist= 0.25 × SL_PCT  → tight trail 25% of SL behind mid
            //   Lock gain  = 0.10 × SL_PCT  → lock entry + 10% of SL as buffer
            //
            // With SL_PCT stored in pos.sl_pct at entry, every instrument gets
            // correctly scaled arms regardless of absolute price level.
            {
                const double sl_pct   = (pos.sl_pct > 0.0) ? pos.sl_pct : SL_PCT;
                const double move_pct = pos.is_long
                    ? (mid - pos.entry) / pos.entry * 100.0
                    : (pos.entry - mid) / pos.entry * 100.0;

                const double lock_arm   = sl_pct * 0.50;
                const double trail1_arm = sl_pct * 1.00;
                const double trail2_arm = sl_pct * 2.00;
                const double trail1_dist = sl_pct * 0.40;
                const double trail2_dist = sl_pct * 0.25;
                const double lock_gain   = sl_pct * 0.10;

                if (move_pct >= trail2_arm) {
                    // Tight trail: 0.25×SL behind mid
                    const double trail = pos.is_long
                        ? mid * (1.0 - trail2_dist / 100.0)
                        : mid * (1.0 + trail2_dist / 100.0);
                    if ( pos.is_long && trail > pos.sl) pos.sl = trail;
                    if (!pos.is_long && trail < pos.sl) pos.sl = trail;
                } else if (move_pct >= trail1_arm) {
                    // Standard trail: 0.40×SL behind mid
                    const double trail = pos.is_long
                        ? mid * (1.0 - trail1_dist / 100.0)
                        : mid * (1.0 + trail1_dist / 100.0);
                    if ( pos.is_long && trail > pos.sl) pos.sl = trail;
                    if (!pos.is_long && trail < pos.sl) pos.sl = trail;
                } else if (move_pct >= lock_arm) {
                    // Lock breakeven + 0.10×SL buffer
                    const double be = pos.is_long
                        ? pos.entry * (1.0 + lock_gain / 100.0)
                        : pos.entry * (1.0 - lock_gain / 100.0);
                    if ( pos.is_long && be > pos.sl) pos.sl = be;
                    if (!pos.is_long && be < pos.sl) pos.sl = be;
                }
            }

            // ── PYRAMID ADD-ON ────────────────────────────────────────────────
            // When trail1 arm is crossed and regime is EXPANSION_BREAKOUT or TREND,
            // add one pyramid leg at current price with TP=remaining comp_range×1.6
            // and SL raised to BE (same as Gold's pyramid pattern).
            // One add-on only (pyramid_done). Inherits same TP/SL as main position.
            if (!pos.pyramid_done) {
                const double sl_pct_now = (pos.sl_pct > 0.0) ? pos.sl_pct : SL_PCT;
                const double trail1_arm_pct = sl_pct_now * 1.00;
                const double move_pct_now = pos.is_long
                    ? (mid - pos.entry) / pos.entry * 100.0
                    : (pos.entry - mid) / pos.entry * 100.0;
                if (move_pct_now >= trail1_arm_pct) {
                    if (!pos.pyramid_armed) {
                        pos.pyramid_armed = true;
                        std::cout << "[ENG-" << symbol << "] PYRAMID ARMED"
                                  << " move_pct=" << move_pct_now << "% arm=" << trail1_arm_pct << "%\n";
                        std::cout.flush();
                    }
                }
                // Arm and open pyramid: must be armed, not yet done,
                // and macro regime must be expansion/trend
                if (pos.pyramid_armed && !pos.pyramid_done && macro_regime) {
                    const bool exp_regime =
                        (std::strncmp(macro_regime, "EXPANSION_BREAKOUT", 18) == 0) ||
                        (std::strncmp(macro_regime, "TREND_CONTINUATION", 18) == 0);
                    if (exp_regime) {
                        pos.pyramid_done  = true;
                        pos.pyramid_entry = mid;
                        // Pyramid TP: same as main position TP
                        pos.pyramid_tp = pos.tp;
                        // Pyramid SL: locked to BE (main entry ± small buffer)
                        const double be_buffer = mid * (sl_pct_now * 0.10) / 100.0;
                        pos.pyramid_sl = pos.is_long
                            ? pos.entry + be_buffer
                            : pos.entry - be_buffer;
                        std::cout << "[ENG-" << symbol << "] PYRAMID ADD-ON"
                                  << " entry=" << mid
                                  << " tp=" << pos.pyramid_tp
                                  << " sl=" << pos.pyramid_sl
                                  << " regime=" << macro_regime << "\n";
                        std::cout.flush();
                        // Fire the pyramid as a signal so main.cpp can send the order
                        // We set a flag in the OpenPos — main dispatch will see it
                        // on the NEXT tick and send the order.
                        // (Returning a signal here would interfere with pos management)
                    }
                }
            }
            // Manage pyramid position TP/SL
            if (pos.pyramid_done && pos.pyramid_entry > 0.0) {
                const bool pyr_tp = pos.is_long ? (bid >= pos.pyramid_tp) : (ask <= pos.pyramid_tp);
                const bool pyr_sl = pos.is_long ? (bid <= pos.pyramid_sl) : (ask >= pos.pyramid_sl);
                if (pyr_tp) {
                    std::cout << "[ENG-" << symbol << "] PYRAMID TP hit=" << pos.pyramid_tp << "\n";
                    std::cout.flush();
                    pos.pyramid_entry = 0.0;  // close pyramid tracking
                }
                else if (pyr_sl) {
                    std::cout << "[ENG-" << symbol << "] PYRAMID SL hit=" << pos.pyramid_sl << "\n";
                    std::cout.flush();
                    pos.pyramid_entry = 0.0;
                }
            }

            // ── BREAKOUT FAILURE SCRATCH ──────────────────────────────────────
            // If within first 120s the price moves against us by > 0.08% of entry,
            // the breakout is confirmed false — cut immediately. Do NOT hold a
            // wrong-direction break for 20 minutes hoping for reversal.
            // Data: USTEC SHORT held 20min at -$20 vs SL $100 away. Scratch saves
            // ~$10-15 by exiting at first confirmation of failure (~60-90s in).
            // 0.08% chosen: above spread noise (0.02-0.04%) but tight enough to
            // catch a genuine false break within the first few minutes.
            {
                const int64_t held_sec = nowSec() - pos.entry_ts;
                if (held_sec <= 120) {
                    const bool is_oil_symbol =
                        (std::strcmp(symbol, "USOIL.F") == 0) ||
                        (std::strcmp(symbol, "BRENT") == 0);
                    // Raised scratch threshold: 0.12% was within normal tick noise for
                    // liquid index futures, causing valid breakouts to be scratched on
                    // a brief retest. 0.20% (indices) and 0.25% (oil) are above
                    // typical 1-2 minute noise while still catching genuine false breaks.
                    const double scratch_limit = is_oil_symbol ? 0.25 : 0.20;
                    const double adverse_pct = pos.is_long
                        ? (pos.entry - mid) / pos.entry * 100.0
                        : (mid - pos.entry) / pos.entry * 100.0;
                    if (adverse_pct > scratch_limit) {
                        std::cout << "[SCRATCH] " << symbol
                                  << (pos.is_long ? " LONG" : " SHORT")
                                  << " false breakout — adverse=" << adverse_pct
                                  << "% limit=" << scratch_limit
                                  << "% in " << held_sec << "s\n";
                        closePos(mid, "SCRATCH", latency_ms, macro_regime, on_close);
                        return {};
                    }
                }
                // SHADOW quality guard: do not let losing trades drift into timeout.
                // If a trade is still negative after 45s by >0.05%, cut it.
                if (AGGRESSIVE_SHADOW && held_sec >= 45) {
                    const bool is_oil_symbol =
                        (std::strcmp(symbol, "USOIL.F") == 0) ||
                        (std::strcmp(symbol, "BRENT") == 0);
                    const double shadow_cut_limit = is_oil_symbol ? 0.08 : 0.05;
                    const double adverse_pct = pos.is_long
                        ? (pos.entry - mid) / pos.entry * 100.0
                        : (mid - pos.entry) / pos.entry * 100.0;
                    if (adverse_pct > shadow_cut_limit) {
                        std::cout << "[SHADOW-CUT] " << symbol
                                  << (pos.is_long ? " LONG" : " SHORT")
                                  << " adverse=" << adverse_pct
                                  << "% held=" << held_sec << "s\n";
                        closePos(mid, "SHADOW_CUT", latency_ms, macro_regime, on_close);
                        return {};
                    }
                }
            }

            if (nowSec() - pos.entry_ts >= static_cast<int64_t>(MAX_HOLD_SEC)) {
                // Cap timeout exit at SL if price has blown through — mirrors the
                // GoldStack fix. Sparse ticks on reconnect can allow price to pass
                // the SL level without triggering the check above. Without this,
                // a 10-25min timeout fills at whatever mid is, not the intended stop.
                double timeout_exit = mid;
                const bool sl_breached = pos.is_long ? (mid < pos.sl) : (mid > pos.sl);
                if (sl_breached) timeout_exit = pos.sl;
                closePos(timeout_exit, "TIMEOUT", latency_ms, macro_regime, on_close);
                return {};
            }
            return {};
        }

        // ── Phase FSM ─────────────────────────────────────────────────────────
        // Hysteresis: two thresholds prevent threshold flickering.
        //   Enter compression:  ratio < COMPRESSION_THRESHOLD (0.80)
        //   Stay  compression:  ratio < COMP_EXIT_THRESHOLD   (0.95)
        //   Exit  compression:  ratio >= COMP_EXIT_THRESHOLD  (0.95)
        const double vol_ratio = (base_vol_pct > 0.0) ? (recent_vol_pct / base_vol_pct) : 999.0;
        const bool can_enter_compression = (base_vol_pct > 0.0) && (vol_ratio < COMPRESSION_THRESHOLD);
        const bool still_compressed      = (base_vol_pct > 0.0) && (vol_ratio < COMP_EXIT_THRESHOLD);

        if (phase == Phase::FLAT) {
            if (m_comp_reentry_wait > 0) {
                --m_comp_reentry_wait;
                return {};
            }
            // ── SUPERVISOR GATE (Leak 1 fix) ─────────────────────────────────
            // can_enter=false means supervisor says no-trade (or session/daily-loss gate).
            // Do NOT enter compression — stay FLAT until supervisor clears.
            // This closes the gap where allow=0 was advisory: engines were entering
            // COMPRESSION freely and only hit the supervisor check at final execution.
            if (!can_enter) {
                static thread_local int64_t s_last_sup_block_log = 0;
                const int64_t now_s = nowSec();
                if (now_s - s_last_sup_block_log >= 10) {
                    s_last_sup_block_log = now_s;
                    std::cout << "[ENG-" << symbol
                              << "] FLAT: supervisor gate — compression blocked (can_enter=0)\n";
                    std::cout.flush();
                }
                return {};
            }
            // ── GOLD-EQUIVALENT RISK GATES ────────────────────────────────────
            // Global SL cooldown: any recent stop → wait before new compression
            if (nowSec() < m_sl_cooldown_until) return {};

            // VWAP dislocation gate: don't enter near VWAP (mean-reversion zone)
            if (VWAP_MIN_DIST_PCT > 0.0 && m_vwap > 0.0) {
                const double vwap_dist_pct = std::fabs(mid - m_vwap) / mid * 100.0;
                if (vwap_dist_pct < VWAP_MIN_DIST_PCT) return {};
            }

            if (can_enter_compression) {
                phase                  = Phase::COMPRESSION;
                comp_high              = mid;
                comp_low               = mid;
                m_compression_ticks    = 1;
                m_comp_violation_ticks = 0;
                std::cout << std::fixed << std::setprecision(5)
                          << "[ENG-" << symbol << "] COMPRESSION entered"
                          << " ratio=" << vol_ratio
                          << " recent=" << recent_vol_pct << "% base=" << base_vol_pct << "%"
                          << " mid=" << mid << "\n";
                std::cout.unsetf(std::ios::fixed);
                std::cout.flush();
            }
            return {};
        }

        // ── COMPRESSION phase ─────────────────────────────────────────────────
        if (phase == Phase::COMPRESSION) {
            // ── SUPERVISOR GATE (Leak 2 fix) ─────────────────────────────────
            // Supervisor flipped to no-trade while engine was already in COMPRESSION.
            // Abort back to FLAT immediately — do not continue building setup.
            // COMP_REENTRY_DELAY is NOT applied here: this was an external veto, not a
            // failed setup. Engine should re-arm cleanly when supervisor clears.
            if (!can_enter) {
                std::cout << "[ENG-" << symbol
                          << "] COMPRESSION aborted — supervisor gate (can_enter=0)\n";
                std::cout.flush();
                phase                  = Phase::FLAT;
                m_compression_ticks    = 0;
                m_comp_violation_ticks = 0;
                m_comp_initial_range   = 0.0;
                return {};
            }
            // Always extend range — price probing the boundary is part of structure
            if (mid > comp_high) comp_high = mid;
            if (mid < comp_low)  comp_low  = mid;

            const double comp_range   = comp_high - comp_low;
            const double comp_midpt   = (comp_high + comp_low) * 0.5;

            // ── Check three reset conditions ──────────────────────────────────
            // Reset only when ALL THREE are true for COMP_VIOLATION_TICKS consecutive ticks.
            // Single-condition or single-tick violations are absorbed.

            // Condition 1: vol ratio has genuinely expanded (hysteresis exit threshold)
            const bool vol_broken = !still_compressed;  // ratio >= 0.95

            // Condition 2: range has expanded beyond tolerance
            // Use initial comp range if available, else current. We track it separately.
            const bool range_broken = (m_comp_initial_range > 0.0) &&
                                      (comp_range > m_comp_initial_range * COMP_RANGE_TOLERANCE);

            // Condition 3: price has drifted too far from the compression midpoint
            const double drift     = std::fabs(mid - comp_midpt);
            const double max_drift = (comp_range > 0.0) ? (comp_range * COMP_DRIFT_TOLERANCE) : 999.0;
            const bool drift_broken = (drift > max_drift);

            const bool all_violated = vol_broken && range_broken && drift_broken;

            // Min life: ignore ALL violations for first COMP_MIN_LIFE_TICKS ticks.
            // Prevents immediate churn right after entering compression.
            if (m_compression_ticks < COMP_MIN_LIFE_TICKS) {
                ++m_compression_ticks;
                m_comp_violation_ticks = 0;  // can't violate during min life
                // Record initial range once we have a few ticks of data
                if (m_compression_ticks == 3 && m_comp_initial_range <= 0.0)
                    m_comp_initial_range = std::max(comp_range, MIN_COMP_RANGE * 0.5);
                return {};
            }

            ++m_compression_ticks;

            if (all_violated) {
                ++m_comp_violation_ticks;
                if (m_comp_violation_ticks < COMP_VIOLATION_TICKS) {
                    return {};  // absorb — need COMP_VIOLATION_TICKS consecutive violations
                }
                // Genuine structure break — reset
                std::cout << std::fixed << std::setprecision(5)
                          << "[ENG-" << symbol << "] COMPRESSION broken"
                          << " ratio=" << vol_ratio
                          << " range=" << comp_range
                          << " init_range=" << m_comp_initial_range
                          << " drift=" << drift
                          << " ticks=" << m_compression_ticks << "\n";
                std::cout.unsetf(std::ios::fixed);
                std::cout.flush();
                phase                  = Phase::FLAT;
                m_compression_ticks    = 0;
                m_comp_violation_ticks = 0;
                m_comp_initial_range   = 0.0;
                m_comp_reentry_wait    = COMP_REENTRY_DELAY;
                return {};
            }

            // Not all conditions violated — reset violation counter
            m_comp_violation_ticks = 0;

            // ── Arm check ─────────────────────────────────────────────────────
            // Arm when ratio is still well inside compression (< 0.75).
            // Previous logic armed at the 0.80–0.95 boundary — by then vol had
            // already started expanding and the breakout was often already in motion.
            // Arming at < 0.75 means we're ARMED while price is still coiling,
            // ready to fire the moment it breaks — not after.
            if (vol_ratio >= 0.75) {
                return {};  // still building — wait for deeper compression before arming
            }

            // Require minimum range
            if (MIN_COMP_RANGE > 0.0 && comp_range < MIN_COMP_RANGE) {
                std::cout << std::fixed << std::setprecision(5)
                          << "[ENG-" << symbol << "] ARM_CHECK fail reason=range_too_small"
                          << " range=" << comp_range
                          << " min=" << MIN_COMP_RANGE
                          << " hi=" << comp_high << " lo=" << comp_low
                          << " ticks=" << m_compression_ticks << "\n";
                std::cout.unsetf(std::ios::fixed);
                std::cout.flush();
                return {};  // stay — range may grow
            }

            // ── ARM ───────────────────────────────────────────────────────────
            phase                  = Phase::BREAKOUT_WATCH;
            m_compression_ticks    = 0;
            m_comp_violation_ticks = 0;
            m_comp_initial_range   = 0.0;
            m_watch_start_ts       = nowSec();
            watch_ticks            = 0;
            std::cout << std::fixed << std::setprecision(5)
                      << "[ENG-" << symbol << "] BREAKOUT_WATCH started (ARMED)"
                      << " hi=" << comp_high << " lo=" << comp_low
                      << " range=" << comp_range
                      << " ratio=" << vol_ratio
                      << " timeout=" << WATCH_TIMEOUT_SEC << "s\n";
            std::cout.unsetf(std::ios::fixed);
            std::cout.flush();
            // Fall through to BREAKOUT_WATCH check on this same tick
        }

        // ── BREAKOUT_WATCH phase ──────────────────────────────────────────────
        if (phase == Phase::BREAKOUT_WATCH) {
            // Trigger OUTSIDE the range with spread*0.5 tolerance.
            // Previous trigger (comp_high - range*0.15) fired INSIDE the range,
            // giving breakout_move = mid - comp_high = negative. That made net_move
            // always negative → edge always failed. Fix: require mid to clear the
            // level by spread*0.5 so move is always positive and meaningful.
            const double tol        = spread * 0.5;
            const bool   long_break  = (mid >= comp_high + tol);
            const bool   short_break = (mid <= comp_low  - tol);

            if (!long_break && !short_break) {
                const int64_t elapsed = nowSec() - m_watch_start_ts;
                if (elapsed > WATCH_TIMEOUT_SEC) {
                    // structure_gone: vol has fully normalised well above baseline.
                    // Threshold must be >> COMPRESSION_THRESHOLD (0.80-0.85) to avoid
                    // resetting the engine the moment compression ends (vol ~0.85 baseline).
                    // Only reset when vol is clearly back to full expansion (1.5× baseline).
                    const bool structure_gone = (base_vol_pct > 0.0) &&
                                                (recent_vol_pct > base_vol_pct * 1.50);
                    if (structure_gone) {
                        std::cout << "[ENG-" << symbol << "] WATCH timeout — structure gone"
                                  << " rv=" << recent_vol_pct << "% bv=" << base_vol_pct
                                  << "% elapsed=" << elapsed << "s, resetting\n";
                        std::cout.flush();
                        phase = Phase::FLAT;
                        m_break_confirm_ticks = 0;
                    } else {
                        // Stay ARMED — extend watch window, keep same comp_high/comp_low
                        m_watch_start_ts = nowSec();
                        std::cout << "[ENG-" << symbol << "] WATCH extended — structure intact"
                                  << " hi=" << comp_high << " lo=" << comp_low
                                  << " rv=" << recent_vol_pct << "% bv=" << base_vol_pct << "%\n";
                        std::cout.flush();
                    }
                }
                return {};
            }

            // Price has exited the compression range — confirmed breakout
            std::cout << std::fixed << std::setprecision(5)
                      << "[ENG-" << symbol << "] BREAKOUT attempt"
                      << (long_break?" LONG":" SHORT")
                      << " mid=" << mid << " hi=" << comp_high << " lo=" << comp_low
                      << " can_enter=" << can_enter << "\n";
            std::cout.unsetf(std::ios::fixed);
            std::cout.flush();

            const bool is_long = long_break;

            // ── GATE 1: Session/position gate ────────────────────────────────
            if (!can_enter) {
                // can_enter=false = position/session/daily-loss gate.
                // Don't reset phase — stay ARMED, wait for gate to clear.
                return {};
            }

            // ── GATE 2: Spread/instrument gate ───────────────────────────────
            // Instrument-specific: spread too wide, EIA window for oil, etc.
            // Non-resetting — spread may tighten on next tick.
            if (!static_cast<Derived*>(this)->shouldTrade(bid, ask, spread_pct, latency_ms)) {
                std::cout << "[ENG-" << symbol << "] BLOCKED: spread/instrument"
                          << " spread=" << spread_pct << "%\n";
                std::cout.flush();
                return {};  // wait, don't reset
            }

            // ── GATE 3: Rate limiter ──────────────────────────────────────────
            {
                const int64_t now_sec = nowSec();
                while (!m_trade_times.empty() && now_sec - m_trade_times.front() > 60)
                    m_trade_times.pop_front();
                if (static_cast<int>(m_trade_times.size()) >= MAX_TRADES_PER_MIN) {
                    std::cout << "[ENG-" << symbol << "] BLOCKED: rate_limit"
                              << " trades_in_60s=" << m_trade_times.size() << "\n";
                    std::cout.flush();
                    phase = Phase::FLAT; return {};
                }
            }

            // ── GATE 4: Min gap ───────────────────────────────────────────────
            {
                const int64_t now_ts = nowSec();
                // Same-level re-entry guard: don't re-enter within SL_PCT band of recent exit
            // for SAME_LEVEL_SEC seconds. Prevents re-entering the same failed setup.
            if (m_last_exit_price > 0.0 && mid > 0.0 &&
                (now_ts - m_last_exit_ts) < SAME_LEVEL_SEC) {
                const double band = mid * SL_PCT / 100.0 * SAME_LEVEL_BAND_MULT;
                if (std::fabs(mid - m_last_exit_price) < band) {
                    return {};
                }
            }
            if (now_ts - m_last_signal_ts < static_cast<int64_t>(MIN_GAP_SEC)) {
                    std::cout << "[ENG-" << symbol << "] BLOCKED: min_gap"
                              << " gap=" << (now_ts-m_last_signal_ts) << "s min=" << MIN_GAP_SEC << "\n";
                    std::cout.flush();
                    phase = Phase::FLAT; return {};
                }
            }

            const int64_t now = nowSec(); // used by entry block below

            // ── GATE 5: Confirmation move (tick-sustained) ────────────────────
            // Require price to clear the level by comp_range*0.25 AND hold
            // outside for MIN_CONFIRM_TICKS consecutive ticks before firing.
            // Single-tick clearance (old behaviour) fired on sweep spikes —
            // London open liquidity sweep hit comp_high + tolerance in 1 tick,
            // SL hit in 19s. MIN_CONFIRM_TICKS=3 means the spike must sustain
            // for ~0.3-0.9s at London tick rates before a signal is generated.
            // Counter resets to 0 if price pulls back inside the boundary.
            {
                const double comp_range_now = comp_high - comp_low;
                const double confirm        = comp_range_now * 0.25;
                const double clearance      = is_long ? (mid - comp_high) : (comp_low - mid);
                if (clearance < confirm) {
                    m_break_confirm_ticks = 0;  // pulled back inside — reset counter
                    return {};  // wait — stay ARMED, don't reset
                }
                // Price is outside with sufficient clearance — accumulate ticks
                if (MIN_CONFIRM_TICKS > 0) {
                    ++m_break_confirm_ticks;
                    if (m_break_confirm_ticks < MIN_CONFIRM_TICKS) {
                        return {};  // not yet sustained — stay ARMED, wait
                    }
                    // Sustained for MIN_CONFIRM_TICKS — confirmed, fall through
                    m_break_confirm_ticks = 0;
                }
            }

            // ── GATE 6: Edge model ────────────────────────────────────────────
            const double breakout_move = is_long
                ? (mid - comp_high) : (comp_low - mid);
            double momentum_pct = 0.0;
            if (static_cast<int>(m_momentum_window.size()) >= MOMENTUM_WINDOW)
                momentum_pct = (mid - m_momentum_window.front())
                               / m_momentum_window.front() * 100.0;

            const EdgeResult edge = compute_edge_and_execution(
                mid, spread,
                breakout_move, comp_high, comp_low,
                recent_vol_pct, momentum_pct,
                is_long, ACCOUNT_EQUITY, EDGE_CFG);

            if (!edge.valid) {
                const double comp_range_now = comp_high - comp_low;
                const double net_approx     = breakout_move - spread * EDGE_CFG.cost_spread_mult;
                const double bp_approx      = (mid > 0.0) ? (net_approx / mid * 10000.0) : 0.0;
                const double need_pts       = comp_range_now * EDGE_CFG.min_range_factor;
                std::cout << std::fixed << std::setprecision(4)
                          << "[ENG-" << symbol << "] BLOCKED: edge_model"
                          << " move=" << breakout_move
                          << " range=" << comp_range_now
                          << " spread=" << spread
                          << " net=" << net_approx
                          << " bp=" << bp_approx
                          << " need_pts=" << need_pts
                          << " need_bp=" << EDGE_CFG.min_edge_bp << "\n";
                std::cout.unsetf(std::ios::fixed);
                std::cout.flush();
                phase = Phase::FLAT; return {};
            }

            std::cout << "[ENG-" << symbol << "] EDGE OK"
                      << " expected=" << edge.expected_move
                      << " cost=" << edge.total_cost
                      << " net=" << edge.net_edge
                      << " bs=" << edge.breakout_strength
                      << " tp=" << edge.tp_price
                      << " sl=" << edge.sl_price
                      << " size=" << edge.size << "\n";
            std::cout.flush();

            // Side-specific chop gate: if this direction has been paused, skip
            {
                const int side_idx = is_long ? 0 : 1;
                if (nowSec() < m_side_pause_until[static_cast<size_t>(side_idx)]) {
                    std::cout << "[ENG-" << symbol << "] CHOP-PAUSE: "
                              << (is_long ? "LONG" : "SHORT") << " side paused\n";
                    std::cout.flush();
                    phase = Phase::FLAT;
                    return {};
                }
            }

            pos.active          = true;
            pos.is_long         = is_long;
            pos.entry           = mid;
            pos.tp              = edge.tp_price;
            pos.sl              = edge.sl_price;
            pos.size            = edge.size;
            pos.sl_pct          = SL_PCT;
            pos.mfe             = 0.0;
            pos.mae             = 0.0;
            pos.entry_ts        = now;
            pos.spread_at_entry = spread;
            strncpy_s(pos.regime, macro_regime ? macro_regime : "", 31);
            // Store supervisor regime at entry for regime-flip exit detection
            strncpy_s(pos.entry_regime, macro_regime ? macro_regime : "", 31);

            m_last_signal_ts = now;
            ++m_trade_id;
            ++signal_count;
            m_trade_times.push_back(now);   // rate limiter: record entry time
            phase = Phase::FLAT;

            BreakoutSignal sig;
            sig.valid            = true;
            sig.is_long          = is_long;
            sig.entry            = mid;
            sig.tp               = edge.tp_price;
            sig.sl               = edge.sl_price;
            sig.reason           = is_long ? "COMP_BREAK_LONG" : "COMP_BREAK_SHORT";
            sig.net_edge         = edge.net_edge;
            sig.breakout_strength= edge.breakout_strength;
            sig.momentum_score   = edge.momentum_score;
            sig.vol_score        = edge.vol_score;

            static_cast<Derived*>(this)->onSignal(sig);
            return sig;
        }  // end BREAKOUT_WATCH

        return {};  // phase not handled (shouldn't reach here)
    }

    void forceClose(double bid, double ask, const char* reason,
                    double latency_ms, const char* macro_regime,
                    CloseCallback on_close) noexcept
    {
        if (!pos.active) return;
        closePos((bid + ask) * 0.5, reason, latency_ms, macro_regime, on_close);
    }

protected:
    std::deque<double> m_prices;
    int64_t            m_last_signal_ts       = 0;
    // Same-level re-entry guard — prevents re-entering same failed compression
    double             m_last_exit_price      = 0.0;
    int64_t            m_last_exit_ts         = 0;
    static constexpr double  SAME_LEVEL_BAND_MULT = 1.0;  // band = SL_PCT × 1.0 of price
    static constexpr int64_t SAME_LEVEL_SEC       = 60;

    // ── Gold-equivalent risk controls (ported from GoldEngineStack) ───────
    // Global SL cooldown: any stop hit → block new entries for GLOBAL_SL_COOLDOWN_SEC
    int64_t            m_sl_cooldown_until    = 0;
    static constexpr int64_t GLOBAL_SL_COOLDOWN_SEC = 120;

    // Side-specific chop detection: 2 SL hits same side in window → pause that side
    // side 0 = LONG, side 1 = SHORT
    std::array<int64_t, 2>             m_side_pause_until{{0, 0}};
    std::array<std::deque<int64_t>, 2> m_side_sl_times;
    static constexpr int64_t SIDE_CHOP_WINDOW_SEC   = 300;
    static constexpr int64_t SIDE_CHOP_PAUSE_SEC    = 300;
    static constexpr int     SIDE_CHOP_TRIGGER      = 2;

    // VWAP tracking — daily cumulative tick average, resets at UTC midnight
    double             m_vwap_cum_pv          = 0.0;  // sum(price)
    double             m_vwap_cum_vol         = 0.0;  // tick count
    double             m_vwap                 = 0.0;  // current VWAP
    int                m_vwap_last_day        = -1;   // last UTC day-of-year
    // VWAP dislocation gate: don't enter within VWAP_MIN_DIST_PCT of VWAP
    // Set to 0 to disable. Calibrated per instrument class at startup.
    double             VWAP_MIN_DIST_PCT      = 0.05; // % of mid — 0.05% default

    // Regime-flip exit: if supervisor regime changes while in trade (>= 60s)
    // close the position — the market structure that justified entry has changed
    static constexpr int64_t REGIME_FLIP_MIN_HOLD_SEC = 60;
    int                m_trade_id             = 0;
    int                m_compression_ticks    = 0;   // ticks spent in COMPRESSION phase
    int                m_comp_violation_ticks = 0;   // consecutive ticks where ALL reset conditions are true
    int                m_comp_reentry_wait    = 0;   // countdown before re-detecting compression after reset
    double             m_comp_initial_range   = 0.0; // comp range captured at tick 3 — used for range_tolerance check
    int                m_break_confirm_ticks  = 0;   // consecutive ticks price has stayed outside comp boundary (Gate 5)

    static int64_t nowSec() noexcept {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    template<typename It>
    static double rangePct(It begin, It end) noexcept {
        double hi = *begin, lo = *begin;
        for (auto it = begin; it != end; ++it) {
            if (*it > hi) hi = *it;
            if (*it < lo) lo = *it;
        }
        const double mid = (hi + lo) * 0.5;
        return (mid > 0.0) ? ((hi - lo) / mid * 100.0) : 0.0;
    }

    void closePos(double exit_px, const char* reason,
                  double latency_ms, const char* macro_regime,
                  CloseCallback& on_close) noexcept
    {
        if (!pos.active) return;

        TradeRecord tr;
        tr.id            = m_trade_id;
        tr.symbol        = symbol;
        tr.side          = pos.is_long ? "LONG" : "SHORT";
        tr.entryPrice    = pos.entry;
        tr.exitPrice     = exit_px;
        tr.tp            = pos.tp;
        tr.sl            = pos.sl;
        tr.size          = pos.size;
        tr.pnl           = (pos.is_long ? (exit_px - pos.entry)
                                        : (pos.entry - exit_px)) * pos.size;
        tr.mfe           = pos.mfe;
        tr.mae           = pos.mae;
        tr.entryTs       = pos.entry_ts;
        tr.exitTs        = nowSec();
        tr.exitReason    = reason;
        tr.spreadAtEntry = pos.spread_at_entry;
        tr.latencyMs     = latency_ms;
        // Use the symbol-derived engine name so typed engines (SpEngine, NqEngine etc)
        // are identifiable in the CSV. Was hardcoded "BreakoutEngine" for all types.
        tr.engine        = std::string(symbol ? symbol : "???") + "_BE";
        tr.regime        = (macro_regime && *macro_regime) ? macro_regime : pos.regime;

        // Record exit for same-level re-entry guard
        m_last_exit_price = exit_px;
        m_last_exit_ts    = nowSec();

        // ── Gold-equivalent SL cooldown + chop detection ─────────────────
        if (std::strcmp(reason, "SL_HIT") == 0) {
            const int64_t now_s = nowSec();
            // Global SL cooldown — block all new entries for GLOBAL_SL_COOLDOWN_SEC
            m_sl_cooldown_until = now_s + GLOBAL_SL_COOLDOWN_SEC;
            std::cout << "[ENG-" << symbol << "] SL_COOLDOWN "
                      << GLOBAL_SL_COOLDOWN_SEC << "s\n";
            std::cout.flush();
            // Side-specific chop detection
            const int side_idx = tr.side == "LONG" ? 0 : 1;
            const size_t u = static_cast<size_t>(side_idx);
            auto& q = m_side_sl_times[u];
            q.push_back(now_s);
            // Trim to window
            while (!q.empty() && now_s - q.front() > SIDE_CHOP_WINDOW_SEC)
                q.pop_front();
            if (static_cast<int>(q.size()) >= SIDE_CHOP_TRIGGER) {
                m_side_pause_until[u] = now_s + SIDE_CHOP_PAUSE_SEC;
                q.clear();
                std::cout << "[ENG-" << symbol << "] CHOP-DETECTED: "
                          << tr.side << " side paused "
                          << SIDE_CHOP_PAUSE_SEC << "s\n";
                std::cout.flush();
            }
        }

        pos.active = false;
        pos        = OpenPos{};

        if (on_close) on_close(tr);
    }
};

// ==============================================================================
// Concrete engine — standard CRTP policy (no extra overrides needed)
// Used for GOLD.F (the gold multi-stack handles the rest).
// Per-instrument typed engines (SpEngine, NqEngine, OilEngine) and MacroContext
// live in SymbolEngines.hpp which includes this file.
// ==============================================================================
class BreakoutEngine final : public BreakoutEngineBase<BreakoutEngine>
{
public:
    explicit BreakoutEngine(const char* sym) noexcept { symbol = sym; }
};

} // namespace omega
