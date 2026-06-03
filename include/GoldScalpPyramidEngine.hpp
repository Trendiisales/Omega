#pragma once
// =============================================================================
// GoldScalpPyramidEngine.hpp -- M5 Gold Scalp with Pyramid + Aggressive Trail
// =============================================================================
//
// 2026-05-18 SESSION DESIGN (Claude / Jo):
//   Dedicated 5-minute XAUUSD scalper optimized for:
//     - 5min to 60min hold time
//     - Aggressive profit locking (4-phase trailing stop)
//     - Pyramiding up to 5 layers on trend continuation
//     - Cost-covered many-small-trades philosophy
//
//   Strategy (Donchian breakout + EMA filter + momentum confirmation):
//     - M5 bars, UTC-aligned to floor(now_s / 300)
//     - Entry: price breaks above/below N-bar Donchian channel
//     - Filter: EMA9 vs EMA21 alignment (trend direction agreement)
//     - Filter: Momentum bar (body > 40% of range, close in correct half)
//     - Session: 07-21 UTC only (London + NY)
//     - ATR floor 1.50 / cap 15.00 (avoid dead and news markets)
//     - Cost gate: TP must cover 1.5x round-trip cost
//
//   L2 ORDER FLOW INTEGRATION (2026-05-18, same session):
//     All L2 signals sourced from MacroContext (FIX -> AtomicL2 -> MacroContext).
//     Degrades gracefully: when l2_real=false, L2 filters are bypassed (no
//     blocking of entries when DOM is stale/unavailable). This is live-only
//     enhancement -- backtests run without L2 and results remain valid.
//
//     5 integration points:
//     1. ENTRY CONFIRMATION: l2_imbalance >= 0.58 (longs) / <= 0.42 (shorts)
//        when l2_real=true. Neutral imbalance (0.42-0.58) still allowed.
//     2. WALL GATE: wall_against blocks entry (wall_above blocks longs,
//        wall_below blocks shorts). Large resting limit = likely rejection.
//     3. PYRAMID ACCELERATION: book_slope confirms + vacuum_with -> lower
//        pyramid threshold by 20%. Wall_against -> block pyramid add.
//     4. DYNAMIC TRAIL: L2 flip against position -> tighten trail to 60%
//        of normal distance. L2 confirm -> allow wider trail (no tighten).
//     5. LOT SIZING: book_slope confirms -> 1.2x base size (capped at LOT_MAX).
//        Wall_against at entry -> 0.7x size.
//
//   Management (the core edge):
//     Phase 1 (BE Lock):     MFE >= cost*2.5 -> SL to entry + cost
//     Phase 2 (Profit Lock): MFE >= ATR*0.4  -> lock 35% of MFE
//     Phase 3 (Tight Trail): MFE >= ATR*0.7  -> trail ATR*trail_tight behind MFE
//     Phase 4 (Very Tight):  MFE >= ATR*1.2  -> trail ATR*trail_tight*0.6 behind
//
//   Pyramiding:
//     Layer 2 at MFE >= 1.0x SL dist, 80% base size
//     Layer 3 at MFE >= 1.5x SL dist, 65% base size
//     Layer 4 at MFE >= 2.0x SL dist, 50% base size
//     Layer 5 at MFE >= 3.0x SL dist, 40% base size
//     All layers close together on trail/SL/TP/time-stop
//
//   S63 protection:
//     LOSS_CUT_PCT  -- cold-loss cut (same pattern as VWAPReversionEngine)
//     BE_ARM_PCT    -- arms BE ratchet
//     BE_BUFFER_PCT -- BE trigger buffer
//
//   Lineage:
//     backtest/gold_scalp_pyramid_bt.cpp: standalone harness with 162-config sweep
//     Designed from htf_bt_minimal.cpp + GoldEngineStack trail patterns
//
// SAFETY:
//   shadow_mode = true by default. Production engine, promote after sweep
//   validation on fresh tape. Respects gold_any_open mutual exclusion.
//
// LOG NAMESPACE:
//   All log lines use prefix [GSP].
//   tr.engine = "GoldScalpPyramid".
//   tr.regime = "M5_SCALP".
// =============================================================================

#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <deque>
#include <functional>
#include <string>
#include "OmegaCostGuard.hpp"
#include "OmegaTradeLedger.hpp"
#include "OHLCBarEngine.hpp"  // S99e: for OHLCBar struct used by prime_from_history()
#include "OpenPositionRegistry.hpp"  // S-2026-06-03: omega::PositionSnapshot for persist

namespace omega {

// =============================================================================
// Exit philosophy (2026-05-19 part-A):
//   Runtime selector for how _manage_position handles the aggressive trail.
//   Default TICK_LEVEL preserves pre-refactor behaviour for live engines.
//
//   TICK_LEVEL      -- 4-phase trail ratchet + trail-hit check every tick.
//                      Hard SL/TP also per-tick. (DEFAULT, live shape.)
//   BAR_CLOSE_ONLY  -- 4-phase trail ratchet + trail-hit check only at bar
//                      close. Hard SL/TP + Phase-1 BE lock + S63 still per-
//                      tick (safety). Closer to the bar-level harness shape
//                      that reported +$15K paper edge.
//   GIVE_BACK       -- Skip 4-phase trail entirely. Once mfe >= ATR*0.4,
//                      exit when (mfe_peak - move) >= TRAIL_TIGHT * mfe_peak
//                      (i.e. retain (1-TRAIL_TIGHT) of MFE). Phase-1 BE lock
//                      still applies for safety. exitReason = "GIVEBACK_HIT".
// =============================================================================
enum class ExitPhilosophy {
    TICK_LEVEL     = 0,
    BAR_CLOSE_ONLY = 1,
    GIVE_BACK      = 2,
};

class GoldScalpPyramidEngine {
public:
    // ---- Timing ---------------------------------------------------------------
    static constexpr int BAR_SECS = 300;  // 5 minutes

    // ---- Donchian lookback (tunable via engine_init.hpp) ----------------------
    int LOOKBACK = 12;  // default, overridden by sweep best

    // ---- Risk parameters (tunable via engine_init.hpp) ------------------------
    double SL_ATR_MULT   = 1.0;   // SL = ATR14 * SL_ATR_MULT
    double TP_ATR_MULT   = 2.0;   // TP = ATR14 * TP_ATR_MULT
    double TRAIL_TIGHT   = 0.20;  // tight trail distance as ATR fraction

    // ---- Exit philosophy (2026-05-19 part-A) ----------------------------------
    // Default TICK_LEVEL = pre-refactor shape. Live engines that don't set
    // this field continue to behave identically. _manage_position branches.
    ExitPhilosophy exit_philosophy = ExitPhilosophy::TICK_LEVEL;

    // ---- Pyramid config -------------------------------------------------------
    bool   PYRAMID_ON    = true;
    static constexpr int MAX_LAYERS = 5;

    // ---- S63 in-flight protection (VWR pattern) --------------------------------
    double LOSS_CUT_PCT  = 0.05;   // XAU@3300: ~$1.65 cold-loss cut
    double BE_ARM_PCT    = 0.03;   // XAU@3300: ~$0.99 mfe arms ratchet
    double BE_BUFFER_PCT = 0.012;  // XAU@3300: ~$0.40 buffer

    // ---- L2 tuning (all thresholds configurable from engine_init.hpp) ----------
    double L2_IMBAL_LONG_MIN  = 0.58;  // min imbalance for long confirmation
    double L2_IMBAL_SHORT_MAX = 0.42;  // max imbalance for short confirmation
    double L2_SLOPE_CONFIRM   = 0.10;  // |book_slope| > this = meaningful pressure
    double L2_TRAIL_TIGHTEN   = 0.60;  // trail multiplier when L2 flips against
    double L2_SIZE_BOOST      = 1.20;  // lot multiplier when slope confirms
    double L2_SIZE_WALL_CUT   = 0.70;  // lot multiplier when wall against
    double L2_PYRAMID_ACCEL   = 0.80;  // pyramid threshold multiplier (lower = easier add)

    // ---- Sizing ---------------------------------------------------------------
    static constexpr double USD_PER_PT_LOT = 100.0;
    static constexpr double RISK_DOLLARS   = 50.0;
    static constexpr double LOT_MIN        = 0.01;
    static constexpr double LOT_MAX        = 0.05;

    // ---- Cost model (tunable via engine_init.hpp) -----------------------------
    // COST_RT_PTS = realised round-trip cost per oz (spread + slip + commish).
    // Phase-1 BE-lock triggers at MFE >= COST_RT_PTS * BE_ARM_COST_MULT and
    // locks SL at entry + COST_RT_PTS so the trade is risk-free + cost-covered.
    // Bumped default 0.50 -> 0.60 (2026-05-26 S38a) to match observed BB gold
    // spread $0.22 + slip $0.30 + commish ~$0.08 at 0.01 lot.
    double COST_RT_PTS        = 0.60;
    double BE_ARM_COST_MULT   = 2.0;   // arm BE-lock when MFE >= COST*this (was 2.5 hardcoded)
    double HALF_SPREAD        = 0.25;

    // ---- Chop / whipsaw filter (S38a 2026-05-26) ------------------------------
    // Kaufman Efficiency Ratio: |close[t] - close[t-N]| / sum(|close diffs|).
    // 1.0 = perfect trend, 0.0 = perfect chop. Block entries when ER < threshold.
    // Stops the engine entering Donchian breakouts that immediately revert in
    // sideways tape -- the dominant loss mode for breakout scalps.
    double CHOP_ER_MIN        = 0.30;
    int    CHOP_ER_LOOKBACK   = 10;    // M5 bars (50 min) for ER computation
    int    CONSEC_BE_FREEZE_N = 3;     // freeze 30min after N consecutive BE_CUT exits

    // ---- ADX directional-strength filter (S38c 2026-05-26) --------------------
    // Wilder ADX(14). < threshold = no sustained direction = chop.
    // Asymmetric to ER -- a trending move with one pullback still scores high
    // (sustained +DM dominance), unlike ER which punishes path-length.
    // Tested: ADX 10 -> -1% PnL but -24% DD on 2yr historicals. Free safety.
    double CHOP_ADX_MIN       = 10.0;  // S38c default after sweep: 10 = free DD reduction
    int    CHOP_ADX_PERIOD    = 14;    // Wilder period

    // ---- S38e (2026-05-26): time-of-day-aware ADX tightening ------------------
    // During known whipsaw windows (London/NY opens), tighten ADX threshold to
    // filter false breakouts. Hardcoded windows match observed loss-prone hours.
    //   07:55-08:15 UTC = London open spike, runs Asian late-buyer stops
    //   13:25-13:45 UTC = NY open spike, runs London noon stops
    // If CHOP_ADX_MIN_OPEN > CHOP_ADX_MIN, the higher threshold applies in
    // those windows. Set OPEN = 0 to disable (= same behavior as before).
    double CHOP_ADX_MIN_OPEN  = 0.0;   // 0 = disabled, recommend 20.0 to filter open spikes

    // ---- S38e dead-hour block: skip 17:00 + 21:00 UTC ------------------------
    // 2yr backtest: hour-21 net PnL -$45 (44% loss rate, 9 trades).
    //              hour-17 net PnL -$36 (29% loss rate, 361 trades, breakeven).
    // Blocking both: -1.7% PnL but +1.4% PF + -7% DD over 2yr. Net positive.
    bool BLOCK_DEAD_HOURS = true;

    // ---- Range-expansion filter (S38d 2026-05-26) -----------------------------
    // Current bar (high-low) / avg(last N bars range) must exceed mult.
    // Chop bars are small; trend bars are big. Independent signal vs body/range.
    // Tested: 1.2x -> +PF +8% / -DD -32% on 2yr / -PnL -15%. Off by default;
    // enable when scaling beyond 0.01 lot where DD matters more.
    double RANGE_EXP_MULT     = 0.0;   // S38d: 0 = off; 1.2 = recommended when on
    int    RANGE_EXP_LB       = 10;    // bars for avg-range window

    // ---- Session / filter constants -------------------------------------------
    static constexpr double ATR_FLOOR      = 1.50;
    static constexpr double ATR_CAP        = 15.0;
    static constexpr double SPREAD_CAP     = 0.80;
    static constexpr int    MAX_HOLD_BARS  = 12;   // 60 minutes on M5
    static constexpr int    COOLDOWN_SEC   = 60;

    // ---- Public state ---------------------------------------------------------
    bool shadow_mode = true;
    bool enabled     = true;

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;
    CloseCallback on_close_cb;
    std::function<void(const std::string&)> cancel_fn;

    // ---- Position tracking ----------------------------------------------------
    struct Layer {
        bool   active = false;
        double entry  = 0.0;
        double size   = 0.0;
    };

    struct LivePos {
        bool    active       = false;
        bool    is_long      = false;
        double  base_entry   = 0.0;
        double  hard_sl      = 0.0;
        double  hard_tp      = 0.0;
        double  trail_sl     = 0.0;
        double  mfe_peak     = 0.0;   // max favorable excursion in points
        double  mfe_price    = 0.0;   // price at MFE peak
        double  mae          = 0.0;
        double  atr_at_entry = 0.0;
        double  spread_at_entry = 0.0;
        int64_t entry_ts     = 0;
        int64_t entry_bar_seq = 0;
        int     bars_held    = 0;
        Layer   layers[MAX_LAYERS];
        int     n_layers     = 0;
        int     next_pyramid_idx = 1;

        double total_size() const {
            double s = 0.0;
            for (int i = 0; i < n_layers; ++i) if (layers[i].active) s += layers[i].size;
            return s;
        }
        double weighted_entry() const {
            double sv = 0.0, ss = 0.0;
            for (int i = 0; i < n_layers; ++i) {
                if (layers[i].active) { sv += layers[i].entry * layers[i].size; ss += layers[i].size; }
            }
            return ss > 0.0 ? sv / ss : 0.0;
        }
    } m_pos;

    bool has_open_position() const noexcept { return m_pos.active; }

    // S-2026-06-03: open-position persistence (resume in-flight trade across
    // restart/deploy). BASE position only -- pyramid layers are NOT persisted
    // (resumes the base trade and re-pyramids live; acceptable). m_pos
    // {active,is_long,base_entry,hard_sl,hard_tp,entry_ts(ms),layers[]}.
    bool persist_save(const char* eng, const char* sym, omega::PositionSnapshot& o) const {
        if (!m_pos.active) return false;
        o.engine = eng; o.symbol = sym; o.side = m_pos.is_long ? "LONG" : "SHORT";
        o.size = m_pos.layers[0].active ? m_pos.layers[0].size : m_pos.total_size();
        o.entry = m_pos.base_entry; o.sl = m_pos.hard_sl; o.tp = m_pos.hard_tp;
        o.entry_ts = m_pos.entry_ts / 1000;
        return true;
    }
    bool persist_restore(const omega::PositionSnapshot& ps) {
        m_pos = LivePos{};
        m_pos.active = true; m_pos.is_long = (ps.side == "LONG");
        m_pos.base_entry = ps.entry; m_pos.hard_sl = ps.sl; m_pos.hard_tp = ps.tp;
        m_pos.trail_sl = ps.sl; m_pos.mfe_price = ps.entry;
        m_pos.entry_ts = ps.entry_ts * 1000;
        m_pos.layers[0] = {true, ps.entry, ps.size};
        m_pos.n_layers = 1; m_pos.next_pyramid_idx = 1;
        return true;
    }

    // ---- M5 bar ---------------------------------------------------------------
    struct M5Bar {
        double open = 0.0, high = 0.0, low = 0.0, close = 0.0;
        int64_t ts_open = 0;
        int n = 0;
    };

    // ---- Public interface: feed every XAUUSD tick -----------------------------
    // L2 fields sourced from MacroContext in tick_gold.hpp dispatch.
    // When l2_real=false, all L2 filters degrade to neutral (no blocking).
    void on_tick(double bid, double ask, int64_t now_ms,
                 bool can_enter,
                 double l2_imbalance, double book_slope,
                 bool vacuum_ask, bool vacuum_bid,
                 bool wall_above, bool wall_below,
                 bool l2_real,
                 const CloseCallback* ext_close = nullptr)
    {
        if (!enabled) return;

        const double mid = (bid + ask) * 0.5;
        const double spread = ask - bid;

        // ----- Bar accumulation -----
        const int64_t bar_ms = BAR_SECS * 1000LL;
        const int64_t anchor = (now_ms / bar_ms) * bar_ms;

        if (m_cur_anchor < 0) {
            // First tick ever
            m_cur_bar = M5Bar{mid, mid, mid, mid, anchor, 1};
            m_cur_anchor = anchor;
        } else if (anchor != m_cur_anchor) {
            // Bar closed -- finalize
            _on_bar_close(m_cur_bar);
            m_cur_bar = M5Bar{mid, mid, mid, mid, anchor, 1};
            m_cur_anchor = anchor;
            ++m_bars_seen;
            // For BAR_CLOSE_ONLY: the next _manage_position invocation in
            // this tick will see this flag, run the 4-phase trail ratchet
            // + trail-hit check once, and clear it. Live live (TICK_LEVEL)
            // ignores it.
            m_bar_just_closed = true;
        } else {
            if (mid > m_cur_bar.high) m_cur_bar.high = mid;
            if (mid < m_cur_bar.low)  m_cur_bar.low  = mid;
            m_cur_bar.close = mid;
            ++m_cur_bar.n;
        }

        // ----- Manage open position (per-tick) -----
        if (m_pos.active) {
            _manage_position(bid, ask, now_ms,
                             l2_imbalance, book_slope,
                             vacuum_ask, vacuum_bid,
                             wall_above, wall_below, l2_real,
                             ext_close);
            return;  // no new entry while holding
        }

        // ----- Entry check (only on bar-close ticks) -----
        // We check m_signal_pending which is set by _on_bar_close
        if (!m_signal_pending) return;
        m_signal_pending = false;

        if (!can_enter) return;
        if (now_ms < m_cooldown_until) return;

        // Validate signal is still valid at tick level
        if (spread > SPREAD_CAP) return;

        // Cost gate
        double tp_dist = m_signal_atr * TP_ATR_MULT;
        if (!::ExecutionCostGuard::is_viable("XAUUSD", spread, tp_dist, LOT_MIN, 1.5)) return;

        // ---- L2 ENTRY FILTERS (live-only, degrade gracefully) ----
        if (l2_real) {
            // 1. Imbalance confirmation: book must lean in trade direction
            //    Neutral zone (0.42-0.58) is allowed -- only contrary book blocks
            if (m_signal_long && l2_imbalance < L2_IMBAL_SHORT_MAX) return;   // book strongly ask-heavy -> no long
            if (!m_signal_long && l2_imbalance > L2_IMBAL_LONG_MIN) return;   // book strongly bid-heavy -> no short

            // 2. Wall gate: large resting limit against us -> likely rejection
            if (m_signal_long && wall_above) return;   // wall above -> price will bounce down
            if (!m_signal_long && wall_below) return;   // wall below -> price will bounce up
        }

        // Open position
        double sl_dist  = m_signal_atr * SL_ATR_MULT;
        double entry_px = m_signal_long ? ask : bid;
        double sl_px    = m_signal_long ? (entry_px - sl_dist) : (entry_px + sl_dist);
        double tp_px    = m_signal_long ? (entry_px + tp_dist) : (entry_px - tp_dist);

        // ---- L2 LOT SIZING (live-only) ----
        double size = RISK_DOLLARS / (sl_dist * USD_PER_PT_LOT);
        if (l2_real) {
            // Book slope confirms direction -> boost size
            bool slope_confirms = (m_signal_long && book_slope > L2_SLOPE_CONFIRM)
                               || (!m_signal_long && book_slope < -L2_SLOPE_CONFIRM);
            if (slope_confirms) size *= L2_SIZE_BOOST;

            // Wall against at entry -> reduce size (even though we passed the wall gate,
            // this handles the case where wall is on same side but not directly blocking)
            bool wall_against = (m_signal_long && wall_above) || (!m_signal_long && wall_below);
            if (wall_against) size *= L2_SIZE_WALL_CUT;

            // Vacuum with us -> slight boost (thin liquidity ahead = fast move likely)
            bool vacuum_with = (m_signal_long && vacuum_ask) || (!m_signal_long && vacuum_bid);
            if (vacuum_with) size *= 1.10;
        }
        size = std::floor(size / 0.01) * 0.01;
        size = std::max(LOT_MIN, std::min(LOT_MAX, size));

        m_pos = LivePos{};
        m_pos.active       = true;
        m_pos.is_long      = m_signal_long;
        m_pos.base_entry   = entry_px;
        m_pos.hard_sl      = sl_px;
        m_pos.hard_tp      = tp_px;
        m_pos.trail_sl     = sl_px;
        m_pos.mfe_peak     = 0.0;
        m_pos.mfe_price    = entry_px;
        m_pos.atr_at_entry = m_signal_atr;
        m_pos.spread_at_entry = spread;
        m_pos.entry_ts     = now_ms;
        m_pos.entry_bar_seq = m_bars_seen;
        m_pos.bars_held    = 0;
        m_pos.layers[0]    = {true, entry_px, size};
        m_pos.n_layers     = 1;
        m_pos.next_pyramid_idx = 1;

        char buf[384];
        snprintf(buf, sizeof(buf),
            "[GSP] OPEN %s entry=%.2f sl=%.2f tp=%.2f size=%.3f atr=%.2f spread=%.2f "
            "l2_imb=%.3f slope=%.3f l2=%s shadow=%s\n",
            m_signal_long ? "LONG" : "SHORT",
            entry_px, sl_px, tp_px, size, m_signal_atr, spread,
            l2_imbalance, book_slope,
            l2_real ? "live" : "stale",
            shadow_mode ? "true" : "false");
        std::printf("%s", buf);
        std::fflush(stdout);
    }

    // =========================================================================
    // Prime indicators from shared M5 bar history (S99e 2026-05-18)
    //
    // Call ONCE at startup after g_bars_gold.m5 has been hydrated by
    // omega_main.hpp (hydrate_from_csv + load_indicators). Feeds every persisted
    // M5 bar through _on_bar_close() with entry-signal logic suppressed
    // (m_in_warmup=true). EMA9/EMA21/ATR14/Donchian buffer become primed
    // immediately, eliminating the ~105min cold-start EMA21 prime window.
    //
    // Same warmup-entry-guard pattern as S102's XauusdFvgEngine fix: indicator
    // updates run, but no live entries fire from warmup-era prices.
    // Returns bars fed.
    // =========================================================================
    // =========================================================================
    // Prime EMA9/EMA21/ATR14 directly from persisted atomic indicator values
    // (S99g 2026-05-18). Use this when g_bars_gold.m5.get_bars() returns empty
    // (load_indicators restored the atomics but bars_ is not populated by load,
    // only by hydrate_from_csv or live add_bar calls).
    //
    // Donchian buffer (m_highs / m_lows) is NOT seeded by this method -- it
    // requires actual bar high/low history. After this prime, Donchian still
    // takes 9 live M5 bars (~45 min) to fill. EMA21 priming wait drops from
    // 1h 45min to ZERO. Net win: engine is firable 60 min sooner than cold.
    //
    // Call this BEFORE prime_from_history so the per-bar feed (if bars_ is
    // populated by hydrate) overwrites cold atomics with the recursion-correct
    // ones. If bars_ is empty, the atomic seed is what we have.
    //
    // last_close: best-effort approximation for ATR's prev_close. Pass the
    // current mid at startup. Drift on the first live bar's TR is bounded by
    // (mid - actual_last_close) which converges in 1 bar of Wilder smoothing.
    // =========================================================================
    void prime_from_atomics(double ema9_val, double ema21_val,
                            double atr14_val, double last_close) noexcept {
        if (!m_ema_inited) {
            m_ema9.init(9);
            m_ema21.init(21);
            m_ema_inited = true;
        }
        bool seeded_any = false;
        if (ema9_val > 0.0) {
            m_ema9.value  = ema9_val;
            m_ema9.count  = m_ema9.period;
            m_ema9.primed = true;
            seeded_any = true;
        }
        if (ema21_val > 0.0) {
            m_ema21.value  = ema21_val;
            m_ema21.count  = m_ema21.period;
            m_ema21.primed = true;
            seeded_any = true;
        }
        if (atr14_val > 0.0) {
            m_atr.value      = atr14_val;
            m_atr.primed     = true;
            m_atr.have_prev  = true;
            m_atr.prev_close = last_close > 0.0 ? last_close : ema9_val;
            seeded_any = true;
        }
        std::printf("[GSP-PRIME-ATOMICS] ema9=%.2f ema21=%.2f atr14=%.2f last_close=%.2f "
                    "seeded=%d (Donchian still primes from live: 9 M5 bars / ~45min)\n",
                    ema9_val, ema21_val, atr14_val, last_close, (int)seeded_any);
        std::fflush(stdout);
    }

    int prime_from_history(const std::deque<OHLCBar>& bars) noexcept {
        if (bars.empty()) {
            std::printf("[GSP-WARMUP] no bars to feed -- cold start (use prime_from_atomics for indicator seed)\n");
            std::fflush(stdout);
            return 0;
        }
        m_in_warmup = true;
        int fed = 0;
        for (const auto& b : bars) {
            const int64_t ts_ms = b.ts_min * 60LL * 1000LL;
            M5Bar mb;
            mb.open    = b.open;
            mb.high    = b.high;
            mb.low     = b.low;
            mb.close   = b.close;
            mb.ts_open = ts_ms;
            mb.n       = 1;
            _on_bar_close(mb);
            ++m_bars_seen;
            ++fed;
        }
        m_in_warmup      = false;
        m_signal_pending = false;  // discard any tail signal from the warmup feed
        m_cur_anchor     = -1;     // next live tick starts a fresh in-progress bar
        std::printf("[GSP-WARMUP] fed=%d M5 bars from shared g_bars_gold.m5 "
                    "ema9=%d ema21=%d atr=%d donch=%d bars_seen=%lld\n",
                    fed, (int)m_ema9.primed, (int)m_ema21.primed,
                    (int)m_atr.primed, (int)m_highs.size(),
                    (long long)m_bars_seen);
        std::fflush(stdout);
        return fed;
    }

private:
    // ---- Bar state ----
    M5Bar   m_cur_bar{};
    int64_t m_cur_anchor = -1;
    int64_t m_bars_seen  = 0;

    // ---- Indicator state ----
    struct EMAState {
        int period = 0; double value = 0.0; double alpha = 0.0;
        int count = 0; bool primed = false;
        void init(int p) { period = p; alpha = 2.0/(p+1.0); value = 0.0; count = 0; primed = false; }
        void push(double v) {
            if (!primed) { value += v; ++count; if (count >= period) { value /= period; primed = true; } }
            else { value = alpha * v + (1.0 - alpha) * value; }
        }
    };
    EMAState m_ema9, m_ema21;
    bool m_ema_inited = false;

    struct ATRState {
        double value = 0.0; bool primed = false;
        double prev_close = 0.0; bool have_prev = false;
        std::deque<double> seed;
        void push(double h, double l, double c) {
            double tr;
            if (!have_prev) { tr = h - l; }
            else { double a=h-l, b=std::fabs(h-prev_close), cc=std::fabs(l-prev_close); tr=std::max(a,std::max(b,cc)); }
            have_prev = true; prev_close = c;
            if (!primed) { seed.push_back(tr); if ((int)seed.size()>=14) { double s=0; for(auto v:seed)s+=v; value=s/14.0; primed=true; } }
            else { value = (value*13.0+tr)/14.0; }
        }
    } m_atr;

    // ---- Donchian channel ----
    std::deque<double> m_highs, m_lows;
    std::deque<double> m_closes;          // S38a: closes for Kaufman ER chop filter
    std::deque<double> m_ranges;          // S38d: bar high-low for range-expansion
    int                m_consec_be_cut = 0;  // S38a: BE_CUT freeze counter

    // S38c: Wilder ADX state
    struct ADXState {
        double tr_smooth = 0.0;
        double pdm_smooth = 0.0;
        double mdm_smooth = 0.0;
        double adx_value = 0.0;
        bool   primed_di = false;
        bool   primed_adx = false;
        int    di_count = 0;
        int    adx_count = 0;
        double prev_high = 0.0, prev_low = 0.0, prev_close = 0.0;
        bool   have_prev = false;
        double seed_tr = 0.0, seed_pdm = 0.0, seed_mdm = 0.0, seed_dx = 0.0;
        void push(double high, double low, double close, int period) {
            if (!have_prev) {
                prev_high = high; prev_low = low; prev_close = close;
                have_prev = true;
                return;
            }
            double up_m = high - prev_high;
            double dn_m = prev_low - low;
            double pdm = (up_m > dn_m && up_m > 0.0) ? up_m : 0.0;
            double mdm = (dn_m > up_m && dn_m > 0.0) ? dn_m : 0.0;
            double tr = std::max(high - low,
                                 std::max(std::fabs(high - prev_close),
                                          std::fabs(low - prev_close)));
            if (!primed_di) {
                seed_tr += tr; seed_pdm += pdm; seed_mdm += mdm;
                if (++di_count >= period) {
                    tr_smooth = seed_tr; pdm_smooth = seed_pdm; mdm_smooth = seed_mdm;
                    primed_di = true;
                }
            } else {
                tr_smooth  = tr_smooth  - (tr_smooth  / period) + tr;
                pdm_smooth = pdm_smooth - (pdm_smooth / period) + pdm;
                mdm_smooth = mdm_smooth - (mdm_smooth / period) + mdm;
            }
            prev_high = high; prev_low = low; prev_close = close;
            if (primed_di && tr_smooth > 1e-9) {
                double pdi = 100.0 * pdm_smooth / tr_smooth;
                double mdi = 100.0 * mdm_smooth / tr_smooth;
                double sum = pdi + mdi;
                if (sum > 1e-9) {
                    double dx = 100.0 * std::fabs(pdi - mdi) / sum;
                    if (!primed_adx) {
                        seed_dx += dx;
                        if (++adx_count >= period) {
                            adx_value = seed_dx / period;
                            primed_adx = true;
                        }
                    } else {
                        adx_value = (adx_value * (period - 1) + dx) / period;
                    }
                }
            }
        }
    } m_adx;

    // ---- Signal state ----
    bool   m_signal_pending = false;
    bool   m_signal_long    = false;
    double m_signal_atr     = 0.0;

    // ---- Cooldown ----
    int64_t m_cooldown_until = 0;

    // ---- Bar-close gate (2026-05-19 part-A) ----
    // Set true in on_tick when bar transitions. Consumed (set false) by
    // _manage_position when exit_philosophy == BAR_CLOSE_ONLY. Unused
    // by TICK_LEVEL / GIVE_BACK branches.
    bool m_bar_just_closed = false;

    // ---- Consecutive S63 loss tracking ----
    int     m_consec_loss_cut = 0;
    int64_t m_consec_block_until = 0;

    // ---- Warmup state (S99e 2026-05-18) ----
    // True only during prime_from_history(). Suppresses entry-signal emission
    // while indicators are being primed from shared M5 bar history. Same shape
    // as the S102 warmup-entry-guard pattern in XauusdFvgEngine.
    bool m_in_warmup = false;

    // =========================================================================
    // Bar close: update indicators, check entry signal
    // =========================================================================
    void _on_bar_close(const M5Bar& bar) {
        if (!m_ema_inited) {
            m_ema9.init(9);
            m_ema21.init(21);
            m_ema_inited = true;
        }
        m_ema9.push(bar.close);
        m_ema21.push(bar.close);
        m_atr.push(bar.high, bar.low, bar.close);

        // Update Donchian channel
        m_highs.push_back(bar.high);
        m_lows.push_back(bar.low);
        m_closes.push_back(bar.close);
        m_ranges.push_back(bar.high - bar.low);            // S38d: range deque for expansion filter
        m_adx.push(bar.high, bar.low, bar.close, CHOP_ADX_PERIOD);  // S38c: ADX update
        const int win = std::max(LOOKBACK + 1,
                       std::max(CHOP_ER_LOOKBACK + 1, RANGE_EXP_LB + 1));
        if ((int)m_highs.size()  > win) { m_highs.pop_front();  m_lows.pop_front(); }
        if ((int)m_closes.size() > win)   m_closes.pop_front();
        if ((int)m_ranges.size() > win)   m_ranges.pop_front();

        // Update bar counter for open position
        if (m_pos.active) {
            m_pos.bars_held = (int)(m_bars_seen - m_pos.entry_bar_seq);
        }

        // ---- Entry signal check ----
        m_signal_pending = false;

        // S99e: skip entry-signal logic when priming from historical bars.
        // Indicator updates above this point still execute so EMAs/ATR/Donchian
        // become primed. Live entries only fire from live ticks after warmup.
        if (m_in_warmup) return;

        if (!m_atr.primed) return;
        if (!m_ema9.primed || !m_ema21.primed) return;
        if ((int)m_highs.size() <= LOOKBACK) return;
        if (m_pos.active) return;

        // Weekend gate
        if (_is_weekend(bar.ts_open)) return;
        // Session filter: 07-21 UTC
        if (!_is_session_active(bar.ts_open)) return;

        // ATR floor/cap
        if (m_atr.value < ATR_FLOOR || m_atr.value > ATR_CAP) return;

        // S38e: dead-hour block. 2yr backtest shows hour-17 (breakeven) and
        // hour-21 (loss) are net negative. Block both outright.
        if (BLOCK_DEAD_HOURS) {
            time_t tt_d = bar.ts_open / 1000;
            struct tm gm_d;
#ifdef _WIN32
            gmtime_s(&gm_d, &tt_d);
#else
            gmtime_r(&tt_d, &gm_d);
#endif
            if (gm_d.tm_hour == 17 || gm_d.tm_hour == 21) return;
        }

        // S38c: Wilder ADX directional-strength gate.
        // ADX < threshold = no sustained directional pressure = chop.
        // S38e: time-of-day-aware -- during London/NY open windows (known
        // whipsaw zones) use CHOP_ADX_MIN_OPEN (tighter) if set, else normal.
        {
            double adx_thresh = CHOP_ADX_MIN;
            if (CHOP_ADX_MIN_OPEN > CHOP_ADX_MIN) {
                time_t tt = bar.ts_open / 1000;
                struct tm gm;
#ifdef _WIN32
                gmtime_s(&gm, &tt);
#else
                gmtime_r(&tt, &gm);
#endif
                int hm = gm.tm_hour * 100 + gm.tm_min;
                // 07:55-08:15 UTC London open, 13:25-13:45 UTC NY open
                bool in_open_window =
                    (hm >= 755  && hm < 815) ||
                    (hm >= 1325 && hm < 1345);
                if (in_open_window) adx_thresh = CHOP_ADX_MIN_OPEN;
            }
            if (adx_thresh > 0.0 && m_adx.primed_adx && m_adx.adx_value < adx_thresh) {
                return;
            }
        }

        // S38d: range-expansion filter.
        // bar range / avg(last N bars range) must exceed mult. Chop bars
        // are small; momentum bars are big. Independent of body/range ratio.
        if (RANGE_EXP_MULT > 0.0 && (int)m_ranges.size() >= RANGE_EXP_LB + 1) {
            const int end = (int)m_ranges.size() - 1;  // current bar
            double sum_r = 0.0;
            for (int k = end - RANGE_EXP_LB; k < end; ++k) sum_r += m_ranges[k];
            double avg_r = sum_r / RANGE_EXP_LB;
            if (avg_r > 1e-9 && m_ranges[end] / avg_r < RANGE_EXP_MULT) return;
        }

        // S38a: Kaufman Efficiency Ratio chop filter.
        // ER = |close[t] - close[t-N]| / sum_i(|close[i] - close[i-1]|)
        // < CHOP_ER_MIN => sideways / whipsaw tape, block entry.
        if (CHOP_ER_MIN > 0.0 && (int)m_closes.size() >= CHOP_ER_LOOKBACK + 1) {
            const int n   = CHOP_ER_LOOKBACK;
            const int end = (int)m_closes.size() - 1;
            const double net = std::fabs(m_closes[end] - m_closes[end - n]);
            double path = 0.0;
            for (int k = end - n + 1; k <= end; ++k) {
                path += std::fabs(m_closes[k] - m_closes[k - 1]);
            }
            const double er = (path > 1e-9) ? (net / path) : 0.0;
            if (er < CHOP_ER_MIN) return;
        }

        // S38a: consecutive BE_CUT freeze. If the last N exits were all
        // BE-stops, the regime is whipping us out post-entry -- step away.
        if (CONSEC_BE_FREEZE_N > 0 && m_consec_be_cut >= CONSEC_BE_FREEZE_N) {
            return;
        }

        // Compute Donchian channel over prior N bars (exclude current)
        double ch_high = -1e18, ch_low = 1e18;
        int count = 0;
        for (int k = 0; k < (int)m_highs.size() - 1 && count < LOOKBACK; ++k) {
            // Walk from oldest to newest-1
        }
        // Actually use the deque properly: elements [0..size-2] are prior bars
        if ((int)m_highs.size() <= LOOKBACK) return;
        for (int k = (int)m_highs.size() - 1 - LOOKBACK; k < (int)m_highs.size() - 1; ++k) {
            if (k < 0) continue;
            if (m_highs[k] > ch_high) ch_high = m_highs[k];
            if (m_lows[k]  < ch_low)  ch_low  = m_lows[k];
        }

        bool bull_break = (bar.close > ch_high);
        bool bear_break = (bar.close < ch_low);
        if (!bull_break && !bear_break) return;

        bool intend_long = bull_break;

        // EMA trend alignment
        if (intend_long  && m_ema9.value <= m_ema21.value) return;
        if (!intend_long && m_ema9.value >= m_ema21.value) return;

        // Momentum bar filter
        double body  = std::fabs(bar.close - bar.open);
        double range = bar.high - bar.low;
        if (range < 0.01) return;
        if (body / range < 0.40) return;
        double mid_price = (bar.high + bar.low) * 0.5;
        if (intend_long  && bar.close < mid_price) return;
        if (!intend_long && bar.close > mid_price) return;

        // Signal is valid -- store for next tick processing
        m_signal_pending = true;
        m_signal_long    = intend_long;
        m_signal_atr     = m_atr.value;
    }

    // =========================================================================
    // Manage position: per-tick SL/TP/trail + pyramid + S63 + L2-adaptive
    // =========================================================================
    void _manage_position(double bid, double ask, int64_t now_ms,
                          double l2_imbalance, double book_slope,
                          bool vacuum_ask, bool vacuum_bid,
                          bool wall_above, bool wall_below, bool l2_real,
                          const CloseCallback* ext_close)
    {
        if (!m_pos.active) return;

        double move = m_pos.is_long
            ? (bid - m_pos.base_entry)
            : (m_pos.base_entry - ask);
        double adverse = -move;

        // Update MFE
        if (move > m_pos.mfe_peak) {
            m_pos.mfe_peak  = move;
            m_pos.mfe_price = m_pos.is_long ? bid : ask;
        }
        // Update MAE
        if (adverse > m_pos.mae) m_pos.mae = adverse;

        // ---- S63 LOSS_CUT (cold-loss protection) ----
        if (LOSS_CUT_PCT > 0.0 && now_ms >= m_consec_block_until) {
            double loss_cut_dist = m_pos.base_entry * LOSS_CUT_PCT / 100.0;
            if (adverse >= loss_cut_dist) {
                _close_position(bid, ask, now_ms, "LOSS_CUT", ext_close);
                m_cooldown_until = now_ms + COOLDOWN_SEC * 1000LL;
                ++m_consec_loss_cut;
                if (m_consec_loss_cut >= 2) {
                    m_consec_block_until = now_ms + 30LL * 60 * 1000LL;  // 30min block after 2 consec
                    m_consec_loss_cut = 0;
                }
                return;
            }
        }

        // ---- S63 BE_RATCHET (giveback prevention) ----
        if (BE_ARM_PCT > 0.0 && BE_BUFFER_PCT >= 0.0) {
            double arm_pts    = m_pos.base_entry * BE_ARM_PCT / 100.0;
            double buffer_pts = m_pos.base_entry * BE_BUFFER_PCT / 100.0;
            if (m_pos.mfe_peak >= arm_pts && move <= buffer_pts) {
                _close_position(bid, ask, now_ms, "BE_CUT", ext_close);
                m_cooldown_until = now_ms + COOLDOWN_SEC * 1000LL;
                m_consec_loss_cut = 0;  // BE_CUT resets consec loss
                ++m_consec_be_cut;       // S38a: BE-cut freeze counter
                if (m_consec_be_cut >= CONSEC_BE_FREEZE_N) {
                    m_consec_block_until = now_ms + 30LL * 60 * 1000LL;  // 30min freeze
                }
                return;
            }
        }

        // ---- L2-adaptive trail factor ----
        // When L2 is live and book flips against our position, tighten
        // the trail distance. When L2 confirms, keep normal trail.
        // When L2 is stale, trail_mult = 1.0 (no effect).
        double trail_mult = 1.0;
        if (l2_real) {
            bool l2_against = (m_pos.is_long && l2_imbalance < L2_IMBAL_SHORT_MAX)
                           || (!m_pos.is_long && l2_imbalance > L2_IMBAL_LONG_MIN);
            bool slope_against = (m_pos.is_long && book_slope < -L2_SLOPE_CONFIRM)
                              || (!m_pos.is_long && book_slope > L2_SLOPE_CONFIRM);
            // Both imbalance AND slope must agree for tightening -- avoids
            // over-triggering on noisy single-signal flips
            if (l2_against && slope_against) {
                trail_mult = L2_TRAIL_TIGHTEN;  // default 0.60 = 40% tighter trail
            }
        }

        // ---- Trail / giveback (branches on exit_philosophy) ----
        // TICK_LEVEL     -- ratchet 4-phase trail every tick (live shape)
        // BAR_CLOSE_ONLY -- ratchet only on bar-close ticks (m_bar_just_closed)
        //                   plus Phase-1 BE lock per tick (safety)
        // GIVE_BACK      -- no aggressive trail. Phase-1 BE lock per tick.
        //                   Giveback rule evaluated below before SL/TP checks.
        const bool do_full_ratchet =
            (exit_philosophy == ExitPhilosophy::TICK_LEVEL) ||
            (exit_philosophy == ExitPhilosophy::BAR_CLOSE_ONLY && m_bar_just_closed);

        if (do_full_ratchet) {
            // ---- 4-phase aggressive trailing stop ----
            double new_trail = m_pos.hard_sl;

            // Phase 1: BE lock at MFE >= cost * 2.5
            if (m_pos.mfe_peak >= COST_RT_PTS * BE_ARM_COST_MULT) {
                double be = m_pos.is_long
                    ? (m_pos.base_entry + COST_RT_PTS)
                    : (m_pos.base_entry - COST_RT_PTS);
                if (m_pos.is_long) new_trail = std::max(new_trail, be);
                else               new_trail = std::min(new_trail, be);
            }

            // Phase 2: Profit lock at 35% of MFE
            if (m_pos.mfe_peak >= m_pos.atr_at_entry * 0.4) {
                double lock = m_pos.mfe_peak * 0.35;
                double lv = m_pos.is_long
                    ? (m_pos.base_entry + lock)
                    : (m_pos.base_entry - lock);
                if (m_pos.is_long) new_trail = std::max(new_trail, lv);
                else               new_trail = std::min(new_trail, lv);
            }

            // Phase 3: Tight trail behind MFE peak (L2-adaptive distance)
            if (m_pos.mfe_peak >= m_pos.atr_at_entry * 0.7) {
                double td = m_pos.atr_at_entry * TRAIL_TIGHT * trail_mult;
                double tl = m_pos.is_long
                    ? (m_pos.mfe_price - td)
                    : (m_pos.mfe_price + td);
                if (m_pos.is_long) new_trail = std::max(new_trail, tl);
                else               new_trail = std::min(new_trail, tl);
            }

            // Phase 4: Very tight at MFE >= ATR * 1.2 (L2-adaptive distance)
            if (m_pos.mfe_peak >= m_pos.atr_at_entry * 1.2) {
                double td = m_pos.atr_at_entry * TRAIL_TIGHT * 0.60 * trail_mult;
                double tl = m_pos.is_long
                    ? (m_pos.mfe_price - td)
                    : (m_pos.mfe_price + td);
                if (m_pos.is_long) new_trail = std::max(new_trail, tl);
                else               new_trail = std::min(new_trail, tl);
            }

            // Ratchet trail (only moves in favorable direction)
            if (m_pos.is_long) m_pos.trail_sl = std::max(m_pos.trail_sl, new_trail);
            else               m_pos.trail_sl = std::min(m_pos.trail_sl, new_trail);
        } else if (exit_philosophy == ExitPhilosophy::BAR_CLOSE_ONLY ||
                   exit_philosophy == ExitPhilosophy::GIVE_BACK) {
            // Phase-1 BE lock applies every tick regardless (safety / risk-
            // free zone). The aggressive Phase-2/3/4 trail is what gets
            // deferred or replaced under these philosophies.
            if (m_pos.mfe_peak >= COST_RT_PTS * BE_ARM_COST_MULT) {
                double be = m_pos.is_long
                    ? (m_pos.base_entry + COST_RT_PTS)
                    : (m_pos.base_entry - COST_RT_PTS);
                if (m_pos.is_long) m_pos.trail_sl = std::max(m_pos.trail_sl, be);
                else               m_pos.trail_sl = std::min(m_pos.trail_sl, be);
            }
        }
        // Consume bar-close flag (whether ratchet ran or not)
        m_bar_just_closed = false;

        // ---- GIVE_BACK exit rule ----
        // Once MFE >= ATR*0.4 (same arm as Phase-2 in tick-level), exit when
        // (mfe_peak - current_move) >= TRAIL_TIGHT * mfe_peak, i.e. retain
        // (1 - TRAIL_TIGHT) of the favourable excursion. Reuses TRAIL_TIGHT
        // as the giveback fraction so tighter = smaller value (consistent
        // semantics with tick-level trail).
        if (exit_philosophy == ExitPhilosophy::GIVE_BACK) {
            const double arm = m_pos.atr_at_entry * 0.4;
            if (m_pos.mfe_peak >= arm) {
                const double giveback = m_pos.mfe_peak - move;
                if (giveback >= m_pos.mfe_peak * TRAIL_TIGHT) {
                    _close_position(bid, ask, now_ms, "GIVEBACK_HIT", ext_close);
                    m_cooldown_until = now_ms + COOLDOWN_SEC * 1000LL;
                    m_consec_loss_cut = 0;
                    return;
                }
            }
        }

        // ---- Check exits ----
        double eff_sl = m_pos.is_long
            ? std::max(m_pos.hard_sl, m_pos.trail_sl)
            : std::min(m_pos.hard_sl, m_pos.trail_sl);

        // SL / Trail hit
        if (m_pos.is_long && bid <= eff_sl) {
            const char* reason = (m_pos.trail_sl > m_pos.hard_sl) ? "TRAIL_HIT" : "SL_HIT";
            _close_position(bid, ask, now_ms, reason, ext_close);
            m_cooldown_until = now_ms + COOLDOWN_SEC * 1000LL;
            if (std::string(reason) == "SL_HIT") m_consec_loss_cut = 0;
            if (std::string(reason) == "TRAIL_HIT") m_consec_be_cut = 0;  // S38a: trail profit clears freeze
            return;
        }
        if (!m_pos.is_long && ask >= eff_sl) {
            const char* reason = (m_pos.trail_sl < m_pos.hard_sl) ? "TRAIL_HIT" : "SL_HIT";
            _close_position(bid, ask, now_ms, reason, ext_close);
            m_cooldown_until = now_ms + COOLDOWN_SEC * 1000LL;
            if (std::string(reason) == "SL_HIT") m_consec_loss_cut = 0;
            if (std::string(reason) == "TRAIL_HIT") m_consec_be_cut = 0;
            return;
        }

        // TP hit
        if (m_pos.is_long && bid >= m_pos.hard_tp) {
            _close_position(bid, ask, now_ms, "TP_HIT", ext_close);
            m_cooldown_until = now_ms + COOLDOWN_SEC * 1000LL;
            m_consec_loss_cut = 0;
            m_consec_be_cut   = 0;  // S38a: profitable exit clears BE-freeze
            return;
        }
        if (!m_pos.is_long && ask <= m_pos.hard_tp) {
            _close_position(bid, ask, now_ms, "TP_HIT", ext_close);
            m_cooldown_until = now_ms + COOLDOWN_SEC * 1000LL;
            m_consec_loss_cut = 0;
            m_consec_be_cut   = 0;
            return;
        }

        // Time stop: MAX_HOLD_BARS
        int bars_held = (int)(m_bars_seen - m_pos.entry_bar_seq);
        if (bars_held >= MAX_HOLD_BARS) {
            _close_position(bid, ask, now_ms, "TIME_STOP", ext_close);
            m_cooldown_until = now_ms + COOLDOWN_SEC * 1000LL;
            return;
        }

        // ---- Pyramid adds (L2-gated) ----
        if (PYRAMID_ON && m_pos.next_pyramid_idx < MAX_LAYERS) {
            static const double pyr_thresh[] = {0.0, 1.0, 1.5, 2.0, 3.0};
            static const double pyr_size_mult[] = {1.0, 0.80, 0.65, 0.50, 0.40};

            double sl_dist = m_pos.atr_at_entry * SL_ATR_MULT;
            int idx = m_pos.next_pyramid_idx;
            double threshold = pyr_thresh[idx] * sl_dist;

            // L2 pyramid acceleration: when book confirms and vacuum clears
            // the path, lower the threshold so we pyramid earlier into momentum
            if (l2_real) {
                bool slope_confirms = (m_pos.is_long && book_slope > L2_SLOPE_CONFIRM)
                                   || (!m_pos.is_long && book_slope < -L2_SLOPE_CONFIRM);
                bool vacuum_with = (m_pos.is_long && vacuum_ask)
                                || (!m_pos.is_long && vacuum_bid);

                if (slope_confirms && vacuum_with) {
                    threshold *= L2_PYRAMID_ACCEL;  // default 0.80 = 20% easier add
                }

                // Wall against blocks pyramid adds -- large resting limit
                // will likely reject the next push, adding here is riskier
                bool wall_against = (m_pos.is_long && wall_above)
                                 || (!m_pos.is_long && wall_below);
                if (wall_against) {
                    // Don't add layers when a wall is in the way
                    // (skip the entire pyramid block for this tick)
                    goto pyramid_done;
                }
            }

            if (m_pos.mfe_peak >= threshold) {
                double base_size = m_pos.layers[0].size;
                double add_size  = base_size * pyr_size_mult[idx];
                add_size = std::max(LOT_MIN, std::min(LOT_MAX, add_size));
                add_size = std::floor(add_size / 0.01) * 0.01;

                double add_entry = m_pos.is_long ? ask : bid;
                m_pos.layers[idx] = {true, add_entry, add_size};
                m_pos.n_layers = idx + 1;
                m_pos.next_pyramid_idx = idx + 1;

                char buf[384];
                snprintf(buf, sizeof(buf),
                    "[GSP] PYRAMID L%d %s entry=%.2f size=%.3f mfe=%.2f "
                    "l2_imb=%.3f slope=%.3f shadow=%s\n",
                    idx + 1, m_pos.is_long ? "LONG" : "SHORT",
                    add_entry, add_size, m_pos.mfe_peak,
                    l2_imbalance, book_slope,
                    shadow_mode ? "true" : "false");
                std::printf("%s", buf);
                std::fflush(stdout);
            }
        }
        pyramid_done:;
    }

    // =========================================================================
    // Close all layers, fire TradeRecord
    //
    // S99h fix (2026-05-18 part C): tr.pnl/mfe/mae are reported in
    // points*lots (NOT USD). trade_lifecycle.hpp:218-224 multiplies them by
    // tick_value_multiplier(tr.symbol) = 100 for XAUUSD to produce USD,
    // before apply_realistic_costs runs. Pre-fix this engine pre-multiplied
    // by USD_PER_PT_LOT internally, so the downstream multiplier ran a
    // second time and every GSP trade was reported 100x larger than reality
    // (e.g. 2026-05-18 12:35 LOSS_CUT showed $1150, actual was $11.50).
    // Reference convention: XauThreeBar30mEngine.hpp:561-562 + 591/594-595.
    // =========================================================================
    void _close_position(double bid, double ask, int64_t now_ms,
                         const char* reason,
                         const CloseCallback* ext_close)
    {
        double exit_px = m_pos.is_long ? bid : ask;

        // Compute total PnL across all layers in points*lots units.
        // The local total_pnl_usd is kept ONLY for the human-readable
        // stdout log line below; it is NOT written into tr.pnl.
        double total_pnl_pts_lots = 0.0;
        double total_size         = 0.0;
        for (int i = 0; i < m_pos.n_layers; ++i) {
            if (!m_pos.layers[i].active) continue;
            double layer_move = m_pos.is_long
                ? (exit_px - m_pos.layers[i].entry)
                : (m_pos.layers[i].entry - exit_px);
            total_pnl_pts_lots += layer_move * m_pos.layers[i].size;
            total_size         += m_pos.layers[i].size;
        }
        const double total_pnl_usd = total_pnl_pts_lots * USD_PER_PT_LOT;

        char buf[512];
        snprintf(buf, sizeof(buf),
            "[GSP] CLOSE %s entry=%.2f exit=%.2f pnl=$%.2f size=%.3f layers=%d "
            "mfe=%.2f mae=%.2f bars=%d reason=%s shadow=%s\n",
            m_pos.is_long ? "LONG" : "SHORT",
            m_pos.weighted_entry(), exit_px, total_pnl_usd, total_size,
            m_pos.n_layers, m_pos.mfe_peak, m_pos.mae,
            (int)(m_bars_seen - m_pos.entry_bar_seq), reason,
            shadow_mode ? "true" : "false");
        std::printf("%s", buf);
        std::fflush(stdout);

        // Fire TradeRecord -- convention is points*lots for pnl/mfe/mae.
        omega::TradeRecord tr;
        tr.engine     = "GoldScalpPyramid";
        tr.symbol     = "XAUUSD";
        tr.side       = m_pos.is_long ? "LONG" : "SHORT";
        tr.regime     = "M5_SCALP";
        tr.entryPrice = m_pos.weighted_entry();
        tr.exitPrice  = exit_px;
        tr.size       = total_size;
        tr.pnl        = total_pnl_pts_lots;                 // pts*lots; downstream mults to USD
        tr.entryTs    = m_pos.entry_ts / 1000LL;            // TradeRecord uses unix seconds
        tr.exitTs     = now_ms / 1000LL;
        tr.exitReason = reason;
        tr.mfe        = m_pos.mfe_peak * total_size;        // pts*lots
        tr.mae        = m_pos.mae      * total_size;        // pts*lots
        tr.spreadAtEntry = m_pos.spread_at_entry;           // needed by apply_realistic_costs
        tr.shadow     = shadow_mode;

        if (ext_close && *ext_close) {
            (*ext_close)(tr);
        } else if (on_close_cb) {
            on_close_cb(tr);
        }

        m_pos = LivePos{};
    }

    // ---- Time helpers ----
    static bool _is_weekend(int64_t ts_ms) {
        const int64_t s   = ts_ms / 1000LL;
        const int     dow = static_cast<int>((s / 86400LL + 3) % 7);
        const int     hr  = static_cast<int>((s % 86400LL) / 3600LL);
        if (dow == 4 && hr >= 20) return true;
        if (dow == 5) return true;
        if (dow == 6 && hr < 22) return true;
        return false;
    }

    static bool _is_session_active(int64_t ts_ms) {
        const int64_t s  = ts_ms / 1000LL;
        const int     hr = static_cast<int>((s % 86400LL) / 3600LL);
        return (hr >= 7 && hr < 21);
    }
};

}  // namespace omega
