#pragma once
// ==============================================================================
// BreakoutEngine — CRTP policy-based compression breakout engine
// One instance per primary symbol (MES, MNQ, MCL).
// CRTP eliminates all virtual dispatch — hot path is fully inlined.
// ==============================================================================
#include <deque>
#include <string>
#include <chrono>
#include <cmath>
#include <iostream>
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
};

// ==============================================================================
// Edge model — signal strength, cost model, exhaustion filter, adaptive TP/SL
// ==============================================================================

struct EdgeConfig {
    double k_impulse      = 2.2;   // breakout_strength multiplier
    double k_vol          = 1.5;   // vol expansion multiplier
    double k_momentum     = 1.8;   // momentum score multiplier
    double slippage_mult  = 1.5;   // slippage as multiple of spread
    double safety_mult    = 0.3;   // required = total_cost * (1 + safety_mult)  [was 1.2 → ~×2.2, now 0.3 → ×1.3]
    double exhaustion_mult= 1.5;   // block if move > comp_range * exhaustion_mult
    double min_breakout_k = 0.15;  // block if move < comp_range * min_breakout_k
    double min_edge_buffer= 0.00001;
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

    // ── Min breakout guard: reject tiny breaks just above range ──────────────
    // Fix 6: block weak fakes — move must be >= comp_range * min_breakout_k
    if (breakout_move < comp_range * cfg.min_breakout_k) return r;

    // ── Exhaustion filter ─────────────────────────────────────────────────────
    if (breakout_move > comp_range * cfg.exhaustion_mult) return r;

    // ── Normalised features ───────────────────────────────────────────────────
    const double breakout_strength = breakout_move / comp_range;

    // Fix 2: normalize compression_quality to price-relative scale.
    // comp_range/mid gives range as fraction of price — consistent cross-symbol.
    // Invert: tighter compression (smaller fraction) → higher quality score.
    const double comp_pct             = (comp_range / mid);
    const double compression_quality  = (comp_pct > 0.0) ? (1.0 / (1.0 + comp_pct)) : 0.0;

    const double momentum_score   = std::fabs(momentum);
    const double vol_score        = recent_vol;

    // Fix 3: multiplicative signal — all factors must be present (alignment required)
    // Additive lets high-vol + zero-momentum pass; multiplicative requires both.
    const double signal_strength =
        breakout_strength *
        (1.0 + vol_score) *
        (1.0 + momentum_score) *
        (1.0 + compression_quality);

    // ── Full cost model — volatility-scaled slippage ──────────────────────────
    // slippage_mult scales with vol expansion: higher vol = worse fills on breakout.
    // At baseline vol, slippage = spread × slippage_mult (base).
    // When vol expands 2× → slippage_mult doubles → cost rises automatically.
    // Prevents marginal trades from passing during volatile/spiky conditions.
    const double vol_ratio      = (recent_vol > 0.0 && comp_range > 0.0)
                                  ? (recent_vol / (comp_range / mid))  // vol relative to range
                                  : 1.0;
    const double dynamic_slip   = cfg.slippage_mult * std::max(1.0, vol_ratio);
    const double slippage_cost  = spread * dynamic_slip;
    const double total_cost     = (spread * 2.0) + slippage_cost;
    const double required       = total_cost * (1.0 + cfg.safety_mult);

    // ── Saturating expected move ──────────────────────────────────────────────
    // Linear projection (signal × comp_range) overestimates strong breakouts.
    // Replace with tanh saturation: output approaches 2×comp_range asymptotically.
    // Weak signal (near 0) → near linear. Strong signal → saturates at ~2× range.
    // This prevents TP from being set far beyond what the market can realistically deliver.
    const double raw_expected   = signal_strength * comp_range;
    const double saturation_cap = comp_range * 2.0;
    const double expected_move  = saturation_cap * std::tanh(raw_expected / saturation_cap);

    // ── Edge check ────────────────────────────────────────────────────────────
    const double net_edge = expected_move - required;
    if (net_edge <= cfg.min_edge_buffer) return r;

    // ── Adaptive TP/SL with clamps ────────────────────────────────────────────
    // Fix 4: clamp TP to avoid overreach when model overestimates;
    //        clamp SL floor to cover spread so we're not stopped out by noise
    const double tp_raw  = expected_move * 0.7;
    const double sl_raw  = expected_move * 0.5;
    const double tp_dist = std::min(tp_raw, comp_range * 1.2);   // cap at 1.2× comp range
    const double sl_dist = std::max(sl_raw, spread * 3.0);        // floor at 3× spread
    r.tp_price = is_long ? mid + tp_dist : mid - tp_dist;
    r.sl_price = is_long ? mid - sl_dist : mid + sl_dist;

    // ── Edge-based position sizing ────────────────────────────────────────────
    const double risk_per_trade = account_equity * 0.002;  // 0.2% risk per trade
    r.size = (sl_dist > 0.0) ? (risk_per_trade / sl_dist) : 0.01;
    if (r.size < 0.01) r.size = 0.01;
    if (r.size > 5.0)  r.size = 5.0;

    r.expected_move     = expected_move;
    r.total_cost        = required;
    r.net_edge          = net_edge;
    r.breakout_strength = breakout_strength;
    r.momentum_score    = momentum_score;
    r.vol_score         = vol_score;
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

    candidates.erase(
        std::remove_if(candidates.begin(), candidates.end(),
            [&](const TradeCandidate& t) {
                return !t.valid || t.score < cfg.min_score_threshold;
            }),
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
    double      COMPRESSION_THRESHOLD = 0.80;
    int         MAX_HOLD_SEC          = 1500;
    int         MIN_GAP_SEC           = 180;
    double      MAX_SPREAD_PCT        = 0.05;
    double      MOMENTUM_THRESH_PCT   = 0.05;
    double      MIN_BREAKOUT_PCT      = 0.25;
    double      MIN_EDGE_PCT          = 0.0;
    double      SLIPPAGE_EST_PCT      = 0.0;
    double      MIN_COMP_RANGE        = 0.0;
    double      ACCOUNT_EQUITY        = 10000.0;
    EdgeConfig  EDGE_CFG;
    int         MAX_TRADES_PER_MIN    = 2;
    double      ENTRY_SIZE            = 0.01;
    bool        AGGRESSIVE_SHADOW     = false;
    const char* symbol                = "???";
    // Fix 1: time-based watch timeout (seconds) replaces tick-count watch_ticks.
    // Per-symbol tuning via symbols.ini. Indices default 300s, FX 180s.
    int         WATCH_TIMEOUT_SEC     = 300;

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
                        (std::strcmp(symbol, "UKBRENT") == 0);
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
                        (std::strcmp(symbol, "UKBRENT") == 0);
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
        const bool in_compression = (base_vol_pct > 0.0) &&
                                    (recent_vol_pct < COMPRESSION_THRESHOLD * base_vol_pct);

        if (phase == Phase::FLAT) {
            if (in_compression) {
                phase               = Phase::COMPRESSION;
                comp_high           = mid;
                comp_low            = mid;
                m_compression_ticks = 1;
                std::cout << "[ENG-" << symbol << "] COMPRESSION entered"
                          << " recent=" << recent_vol_pct << "% base=" << base_vol_pct
                          << "% ratio=" << (base_vol_pct>0?recent_vol_pct/base_vol_pct:0)
                          << " mid=" << mid << "\n";
                std::cout.flush();
            }
            return {};
        }

        // ── COMPRESSION phase: track the range while vol is compressed ────────
        if (phase == Phase::COMPRESSION) {
            if (in_compression) {
                ++m_compression_ticks;
                if (mid > comp_high) comp_high = mid;
                if (mid < comp_low)  comp_low  = mid;
                return {};  // still compressing — keep tracking
            }
            // Compression just ended — require minimum ticks to ensure range is real
            if (m_compression_ticks < 3) {
                // Too few ticks — range is noise (FX sparse tick issue). Reset quietly.
                phase               = Phase::FLAT;
                m_compression_ticks = 0;
                return {};
            }
            // Compression just ended — check range is viable before watching
            const double comp_range_built = comp_high - comp_low;
            if (MIN_COMP_RANGE > 0.0 && comp_range_built < MIN_COMP_RANGE) {
                // Range too small — don't reset, keep building.
                // Reset to COMPRESSION so price continues expanding the range.
                // Throwing away valid structure was killing setups.
                phase               = Phase::COMPRESSION;
                m_compression_ticks = 3;  // keep minimum tick count so we don't re-enter this check immediately
                return {};
            }
            phase               = Phase::BREAKOUT_WATCH;
            m_compression_ticks = 0;
            m_watch_start_ts    = nowSec();
            watch_ticks         = 0;  // not used for timeout, kept for telemetry
            std::cout << "[ENG-" << symbol << "] BREAKOUT_WATCH started (ARMED)"
                      << " hi=" << comp_high << " lo=" << comp_low
                      << " timeout=" << WATCH_TIMEOUT_SEC << "s\n";
            std::cout.flush();
            // Fall through to BREAKOUT_WATCH check on this same tick
        }

        // ── BREAKOUT_WATCH phase: ARMED — wait indefinitely for price to exit ─
        // Fix 1: time-based timeout (not tick-count). Tick rate is non-deterministic.
        // Fix 2: on timeout, stay ARMED with same range — do NOT reset to FLAT.
        //        Compression structure is still valid after the initial watch period.
        //        Only reset if vol has fully normalized (structure genuinely gone).
        if (phase == Phase::BREAKOUT_WATCH) {
            const double min_exit    = spread * 0.5;
            const bool   long_break  = (mid > comp_high + min_exit);
            const bool   short_break = (mid < comp_low  - min_exit);

            if (!long_break && !short_break) {
                const int64_t elapsed = nowSec() - m_watch_start_ts;
                if (elapsed > WATCH_TIMEOUT_SEC) {
                    // Check if compression structure still holds.
                    // If vol has fully expanded back to baseline, range is stale — reset.
                    // If vol still compressed or moderate, stay ARMED with same range.
                    const bool structure_gone = (base_vol_pct > 0.0) &&
                                                (recent_vol_pct > base_vol_pct * 0.95);
                    if (structure_gone) {
                        std::cout << "[ENG-" << symbol << "] WATCH timeout — structure gone"
                                  << " rv=" << recent_vol_pct << "% bv=" << base_vol_pct
                                  << "% elapsed=" << elapsed << "s, resetting\n";
                        std::cout.flush();
                        phase = Phase::FLAT;
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
            std::cout << "[ENG-" << symbol << "] BREAKOUT attempt"
                      << (long_break?" LONG":" SHORT")
                      << " mid=" << mid << " hi=" << comp_high << " lo=" << comp_low
                      << " can_enter=" << can_enter << "\n";
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
                if (now_ts - m_last_signal_ts < static_cast<int64_t>(MIN_GAP_SEC)) {
                    std::cout << "[ENG-" << symbol << "] BLOCKED: min_gap"
                              << " gap=" << (now_ts-m_last_signal_ts) << "s min=" << MIN_GAP_SEC << "\n";
                    std::cout.flush();
                    phase = Phase::FLAT; return {};
                }
            }

            const int64_t now = nowSec(); // used by entry block below

            // ── GATE 5: Edge model ────────────────────────────────────────────
            // Primary quality gate. Prices breakout move vs spread+slippage cost.
            // Incorporates vol expansion, compression quality, and momentum.
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
                std::cout << "[ENG-" << symbol << "] BLOCKED: edge_model"
                          << " move=" << breakout_move
                          << " comp_range=" << (comp_high - comp_low)
                          << " spread=" << spread << "\n";
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
    int64_t            m_last_signal_ts  = 0;
    int                m_trade_id        = 0;
    int                m_compression_ticks = 0;  // ticks spent inside compression phase

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
