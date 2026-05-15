#pragma once
// =============================================================================
// gold_ultimate_integration.hpp -- Wires GoldUltimateStrategy into tick_gold
// =============================================================================
//
// PURPOSE
// -------
// Provides the glue layer between the existing on_tick_gold() dispatcher and
// the new GoldUltimateStrategy orchestrator. This file:
//
//   1. Builds a MarketSnapshot from all available live data sources
//   2. Calls g_gold_ultimate.evaluate_entry() for each engine's fire decision
//   3. Provides manage_position() overlay for open trades
//   4. Hooks on_trade_close for loss management updates
//
// INTEGRATION PATTERN
// -------------------
// Existing engines call their own should_fire() logic internally. This
// integration adds a SECOND gate: the ultimate strategy must ALSO approve.
//
//   Engine logic says FIRE -> gold_ultimate_gate(engine_id, is_long, sl, tp)
//                          -> returns allow/block
//                          -> if allow, engine proceeds with adjusted lot
//                          -> if block, engine logs and skips
//
// This is a PURE OVERLAY. If g_gold_ultimate.enabled == false, the gate
// always returns true (no change to existing behaviour).
//
// USAGE IN tick_gold.hpp
// ----------------------
// At the top of on_tick_gold(), add:
//   gold_ultimate_update_snapshot(bid, ask, now_ms);
//
// Before each engine fires, replace:
//   if (!gold_any_open && <engine_signal>) { fire(); }
// with:
//   if (!gold_any_open && <engine_signal>) {
//       auto decision = gold_ultimate_gate(EngineId::X, is_long, sl, tp);
//       if (decision.allow) { fire_with_lot(decision.lot_size); }
//   }
//
// On each trade close:
//   gold_ultimate_on_close(net_pnl_usd, expected_pnl_usd, now_ms);
//
// THREAD SAFETY
// -------------
// Called only from the engine dispatch worker thread (single-threaded).
//
// PROTECTED CODE INVARIANT
// ------------------------
// This file does NOT modify any core file. It provides helper functions
// that the operator manually wires into tick_gold.hpp when ready to activate.
// =============================================================================

#include "GoldUltimateStrategy.hpp"
#include "GoldHMM.hpp"
#include "OmegaSignalScorer.hpp"
#include "OmegaRegimeAdaptor.hpp"
#include "OHLCBarEngine.hpp"

namespace omega { namespace gold_ultimate {

// =============================================================================
// SNAPSHOT BUILDER -- Assembles MarketSnapshot from live globals
//
// The caller provides bid/ask/now_ms. All other data is read from globals
// that the existing tick_gold.hpp already maintains.
// =============================================================================

// Forward declarations of globals that exist in the codebase.
// These are NOT new -- they are declared in their respective headers and
// defined in main.cpp or their respective .cpp files.
// The integration file just reads from them.

// From globals.hpp / tick_gold.hpp scope:
//   extern GoldHMM g_gold_hmm;
//   extern OmegaSignalScorer g_signal_scorer;
//   extern omega::regime::RegimeAdaptor g_regime_adaptor;
//   extern OHLCBarSet g_bars_gold;
//   extern ... g_gold_stack;

// =============================================================================
// build_snapshot() -- Construct a full MarketSnapshot from live state
//
// This function reads from atomic/volatile globals. It is safe to call
// from the engine dispatch worker thread.
// =============================================================================
struct SnapshotInputs {
    double      bid;
    double      ask;
    int64_t     now_ms;
    // HMM
    int         hmm_state;       // g_gold_hmm.current_state(feat)
    bool        hmm_warmed;      // g_gold_hmm.warmed()
    double      hmm_p_cont;      // g_gold_hmm.p_continuation(feat)
    // Macro
    std::string macro_regime;    // from MacroRegimeDetector
    int         vol_band;        // from OmegaRegimeAdaptor
    // Momentum
    double      ewm_drift;       // g_gold_stack.ewm_drift()
    double      vol_ratio;       // recent_vol / base_vol
    double      atr_pts;         // from bar indicators
    double      rsi14;           // from bar indicators
    // Structure
    bool        higher_highs;    // from price action analysis
    bool        lower_lows;      // from price action analysis
    // Signal
    int         signal_score;    // from g_signal_scorer
    bool        signal_pass;     // score >= threshold
    // Cross-asset
    double      dxy_return;
    double      spx_return;
    // L2
    double      l2_imbalance;
    double      microprice;
};

inline MarketSnapshot build_snapshot(const SnapshotInputs& in) noexcept {
    MarketSnapshot snap;
    snap.bid          = in.bid;
    snap.ask          = in.ask;
    snap.mid          = (in.bid + in.ask) * 0.5;
    snap.spread_pts   = in.ask - in.bid;
    snap.hmm_state    = in.hmm_state;
    snap.hmm_warmed   = in.hmm_warmed;
    snap.hmm_p_cont   = in.hmm_p_cont;
    snap.macro_regime = in.macro_regime;
    snap.vol_band     = in.vol_band;
    snap.ewm_drift    = in.ewm_drift;
    snap.vol_ratio    = in.vol_ratio;
    snap.atr_pts      = in.atr_pts;
    snap.rsi14        = in.rsi14;
    snap.higher_highs = in.higher_highs;
    snap.lower_lows   = in.lower_lows;
    snap.signal_score = in.signal_score;
    snap.signal_pass  = in.signal_pass;
    snap.dxy_return   = in.dxy_return;
    snap.spx_return   = in.spx_return;
    snap.now_ms       = in.now_ms;
    snap.l2_imbalance = in.l2_imbalance;
    snap.microprice   = in.microprice;

    // Derive hour/minute from now_ms
    const int64_t sec = in.now_ms / 1000;
    snap.hour_utc   = static_cast<int>((sec % 86400) / 3600);
    snap.minute_utc = static_cast<int>((sec % 3600) / 60);

    return snap;
}

// =============================================================================
// gold_ultimate_gate() -- The single entry point engines call to get approval
//
// Returns an EntryDecision. If allow=true, the engine fires with the given
// lot_size. If allow=false, the engine logs block_reason and skips.
//
// When g_gold_ultimate.enabled == false, returns allow=true with base_lot
// (existing behaviour preserved -- no change unless operator activates).
// =============================================================================
inline EntryDecision gold_ultimate_gate(
    const MarketSnapshot& snap,
    EngineId engine,
    bool is_long,
    double proposed_sl_pts,
    double proposed_tp_pts) noexcept
{
    if (!g_gold_ultimate.enabled) {
        // Pass-through mode: strategy disabled, all engines fire normally
        EntryDecision d;
        d.allow = true;
        d.is_long = is_long;
        d.engine = engine;
        d.lot_size = 0.01;
        d.sl_pts = proposed_sl_pts;
        d.tp_pts = proposed_tp_pts;
        d.block_reason = "STRATEGY_DISABLED_PASSTHROUGH";
        return d;
    }

    return g_gold_ultimate.evaluate_entry(snap, engine, is_long,
                                           proposed_sl_pts, proposed_tp_pts);
}

// =============================================================================
// gold_ultimate_on_close() -- Called after every gold trade closes
// =============================================================================
inline void gold_ultimate_on_close(double net_pnl_usd,
                                    double expected_pnl_usd,
                                    int64_t now_ms) noexcept {
    if (!g_gold_ultimate.enabled) return;
    g_gold_ultimate.on_trade_close(net_pnl_usd, expected_pnl_usd, now_ms);
}

// =============================================================================
// gold_ultimate_manage() -- Tick-by-tick position management overlay
//
// Returns PositionUpdate with new_sl_px and emergency_close flag.
// The engine should:
//   1. If emergency_close, close immediately at market
//   2. Otherwise, tighten SL to max(current_sl, new_sl_px)
// =============================================================================
inline GoldUltimateStrategy::PositionUpdate gold_ultimate_manage(
    const MarketSnapshot& snap,
    bool is_long,
    double entry_px,
    double current_sl_px,
    double atr_at_entry,
    TradingRegime entry_regime) noexcept
{
    if (!g_gold_ultimate.enabled) {
        GoldUltimateStrategy::PositionUpdate upd;
        upd.new_sl_px = current_sl_px;
        return upd;
    }

    return g_gold_ultimate.manage_open_position(
        is_long, entry_px, current_sl_px, snap.mid,
        atr_at_entry, entry_regime, snap);
}

// =============================================================================
// PRICE STRUCTURE DETECTOR -- determines higher-highs / lower-lows
//
// Uses the last N swing points from the gold M5/M15 bars to determine
// whether price is making higher highs (bullish structure) or lower lows
// (bearish structure).
// =============================================================================
class PriceStructureDetector {
public:
    static constexpr int LOOKBACK = 20;  // bars to track

    struct State {
        bool higher_highs = false;
        bool lower_lows   = false;
        double last_swing_high = 0.0;
        double last_swing_low  = 0.0;
        double prev_swing_high = 0.0;
        double prev_swing_low  = 0.0;
    };

    void on_bar(double high, double low) noexcept {
        highs_[idx_] = high;
        lows_[idx_]  = low;
        idx_ = (idx_ + 1) % LOOKBACK;
        if (count_ < LOOKBACK) ++count_;

        if (count_ < 5) return;  // need at least 5 bars

        _detect_swings();
    }

    State state() const noexcept { return state_; }

private:
    double highs_[LOOKBACK] = {};
    double lows_[LOOKBACK]  = {};
    int    idx_   = 0;
    int    count_ = 0;
    State  state_{};

    void _detect_swings() noexcept {
        // Simple swing detection: a swing high is a bar whose high is higher
        // than both neighbours. Look at the last 5 bars for fresh swings.
        double recent_high = 0.0;
        double recent_low  = 99999.0;

        for (int i = 0; i < count_; ++i) {
            const int pos = ((idx_ - 1 - i) % LOOKBACK + LOOKBACK) % LOOKBACK;
            if (highs_[pos] > recent_high) recent_high = highs_[pos];
            if (lows_[pos] < recent_low)   recent_low  = lows_[pos];
        }

        // Compare current highs/lows with previous cycle
        if (state_.last_swing_high > 0.0) {
            state_.higher_highs = (recent_high > state_.last_swing_high);
        }
        if (state_.last_swing_low > 0.0 && state_.last_swing_low < 99999.0) {
            state_.lower_lows = (recent_low < state_.last_swing_low);
        }

        // Shift: current becomes previous for next cycle
        if (count_ % 5 == 0) {  // Update every 5 bars
            state_.prev_swing_high = state_.last_swing_high;
            state_.prev_swing_low  = state_.last_swing_low;
            state_.last_swing_high = recent_high;
            state_.last_swing_low  = recent_low;
        }
    }
};

// Global price structure detector instance
inline PriceStructureDetector g_price_structure;

// =============================================================================
// ACTIVATION HELPER -- Call once during engine_init to configure and activate
//
// This is the SINGLE function the operator calls to turn on the strategy.
// It sets enabled=true, loads EV stats if available, and logs activation.
// =============================================================================
inline void gold_ultimate_activate(const char* ev_stats_path = nullptr) noexcept {
    auto& strat = g_gold_ultimate;

    // Load EV guard stats if path provided
    if (ev_stats_path && ev_stats_path[0] != '\0') {
        if (strat.ev_guard().load_stats(ev_stats_path)) {
            std::printf("[ULTIMATE] Loaded EV stats from %s\n", ev_stats_path);
        } else {
            std::printf("[ULTIMATE] WARNING: Failed to load EV stats from %s "
                       "(will skip EV gate)\n", ev_stats_path);
        }
    }

    strat.enabled = true;
    std::printf("[ULTIMATE] ============================================\n");
    std::printf("[ULTIMATE] GOLD ULTIMATE STRATEGY ACTIVATED\n");
    std::printf("[ULTIMATE] Cost ratio min:    %.1fx\n", strat.cost_ratio_min);
    std::printf("[ULTIMATE] EV safety margin:  $%.2f\n", strat.ev_safety_margin);
    std::printf("[ULTIMATE] Daily loss cap:    $%.0f\n", strat.daily_loss_cap_usd);
    std::printf("[ULTIMATE] Drawdown shadow:   $%.0f\n", strat.drawdown_shadow_usd);
    std::printf("[ULTIMATE] Max consec losses: %d (pause %dmin)\n",
               strat.max_consec_losses, strat.pause_duration_ms / 60000);
    std::printf("[ULTIMATE] Lot range:         %.2f - %.2f\n",
               strat.min_lot, strat.max_lot);
    std::printf("[ULTIMATE] ATR entry floor:   %.1f\n", strat.atr_entry_floor);
    std::printf("[ULTIMATE] Edge hours:        ");
    for (int h = 0; h < 24; ++h) {
        if (strat.edge_hours[h]) std::printf("%02d ", h);
    }
    std::printf("\n");
    std::printf("[ULTIMATE] ============================================\n");
    std::fflush(stdout);
}

// =============================================================================
// DEACTIVATION HELPER -- Emergency shutdown
// =============================================================================
inline void gold_ultimate_deactivate(const char* reason = "operator") noexcept {
    g_gold_ultimate.enabled = false;
    std::fprintf(stderr,
        "\033[1;31m[ULTIMATE] DEACTIVATED by %s\033[0m\n", reason);
    std::fflush(stderr);
}

}} // namespace omega::gold_ultimate
