#pragma once
//  ADVERSE-PROTECTION: disabled (enabled=false in engine_init.hpp) -- engine HAS full in-flight machinery (hard SL_PTS=0.40 + BE arm BE_ARM_PTS=0.30 ratchet to entry+/-BE_BUFFER_PTS=0.05 + TRAIL_TIGHT_PTS=0.15 + MAX_HOLD_SEC=600 time-stop); protection moot while disabled -- the entry edge is dead, not the exit: 27-cfg/154M-tick sweep PF0.07-0.09 WR7-8% (BB-extreme touches are continuation not reversal on gold M1), CULL_LEDGER 2026-06-01 mean-rev book; verdict owed before re-enable (needs entry-filter redesign + new sweep) (backfill S-2026-06-24n)
// =============================================================================
// BBandScalpEngine.hpp -- M1 Bollinger-band + RSI mean-reversion scalper
// =============================================================================
//
// 2026-05-18 SESSION DESIGN (Claude / Jo):
//   Tick-driven scalper that fires on M1 Bollinger Band extreme touches with
//   RSI confirmation, then locks BE the moment MFE covers cost and trails
//   tight thereafter. The asymmetric-payoff exit design is the same BE-arm +
//   trail-tight machinery the operator validated in QuickScalp; the entry
//   filter swaps the QuickScalp tick-velocity-derived signal (proven
//   structurally counter-predictive on the 2024-2026 gold tape across 124
//   sweep configs) for the simplest possible STRUCTURAL signal: Bollinger
//   Band extreme touch + RSI extreme + ATR-in-range.
//
//   This mirrors the proven structural-signal pattern used by every working
//   engine in this codebase: VWAPReversion (price vs VWAP), IndexFlow
//   (Donchian + EMA), GoldScalpPyramid (Donchian + EMA), MinimalH4Breakout
//   (Donchian). None of those use a velocity / rate-of-change signal as the
//   primary entry filter -- they reference a specific PRICE LEVEL.
//
// ENTRY (tick-level, gated by M1 bar values from g_bars_gold.m1.ind atomics):
//   LONG:
//     - bid <= bb_lower            (price has touched/pierced the lower band)
//     - rsi14_m1 < RSI_OVERSOLD     (default 35) -- momentum oversold confirms
//     - atr14_m1 in [ATR_FLOOR_M1, ATR_CAP_M1]   (default [0.5, 8.0])
//     - spread <= SPREAD_CAP_PTS    (default 0.40)
//     - session 07-21 UTC, no weekend
//     - cooldown elapsed
//     - cost gate: TP distance covers >= COST_COVER_MULT x round-trip cost
//   SHORT: symmetric (ask >= bb_upper, rsi > RSI_OVERBOUGHT default 65)
//
//   The bb_* / rsi14_m1 / atr14_m1 values are fed into on_tick() by the
//   dispatch layer in tick_gold.hpp -- the engine itself does not maintain
//   its own bar accumulator or indicator state. This matches the QuickScalp
//   pattern and is the only design that lets the engine pick up indicator
//   values that are already being maintained by OHLCBarEngine via persisted
//   atomic state. No warmup, no cold-prime window.
//
// EXIT (asymmetric BE-lock payoff -- the half that DID work in QuickScalp):
//   TP = bb_mid at entry time (mean reversion target)
//   SL = entry +/- SL_PTS (default 0.40pt -- structural: if mean fails to
//        revert from this extreme, structure broke)
//   BE arm: MFE >= BE_ARM_PTS (default 0.30) -- SL ratchets to entry +/-
//           BE_BUFFER_PTS (default 0.05), monotonic in favorable direction
//   Trail: after BE arm, trail TRAIL_TIGHT_PTS (default 0.15) behind MFE
//   Time stop: MAX_HOLD_SEC (default 600s -- BB mean-reverts take 5-10min)
//
// L2:
//   Not required. Engine accepts l2_imbalance / l2_real as optional filter
//   args (signature symmetry with QuickScalp / GSP) but does NOT block
//   entry on stale L2. This lets the engine validate on price-only
//   historical tape (Dukascopy combined CSV, 154M ticks 2024-03 .. 2026-04)
//   and run unmodified in live where L2 may or may not be live.
//
// SIZING:
//   Fixed LOT_BASE per trade (default 0.01). Conservative scalp -- many
//   small bets, no pyramid, no risk-pct sizing. Same shape as QuickScalp.
//
// PRIMING:
//   prime_from_atomics() is a no-op-with-log diagnostic. Because indicator
//   state is sourced from g_bars_gold.m1.ind atomics on every tick, this
//   engine has no internal indicator cold-prime window -- it is firable
//   from tick 1 as long as the M1 atomics are live (m1_ready=true after
//   load_indicators or 15 live M1 closes). Method kept for symmetry with
//   GSP / TrendPullback engine_init.hpp wiring patterns.
//
// LOG NAMESPACE:
//   [BBS] prefix on all log lines.
//   tr.engine = "BBandScalp".
//   tr.regime = "BB_RSI_MEANREV".
//
// SAFETY:
//   shadow_mode = true by default. Production engine, promote after sweep
//   validation on fresh tape (backtest/bband_scalp_bt.cpp). Respects
//   gold_any_open mutual exclusion via has_open_position() contribution
//   at the tick_gold dispatch layer.
//
// PRIOR ART JUSTIFICATION:
//   Today's QuickScalp sweep (2 grids, 124 configs total) proved tick-
//   velocity signals are structurally counter-predictive on mean-reverting
//   gold M1 tape. WR 15-30%, PF 0.12-0.30, Scratch=0 everywhere -- the
//   exit machinery never armed because the entry was already adverse.
//   This engine reuses that SAME exit machinery but with a structural
//   entry filter (BB+RSI mean-revert) which has plausible industry-
//   standard edge expectations: WR 55-65%, PF 1.15-1.40, Scratch > 0
//   on a meaningful fraction.
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

namespace omega {

class BBandScalpEngine {
public:
    // ---- RSI confirmation thresholds (tunable via engine_init.hpp) -----------
    double RSI_OVERSOLD     = 35.0;   // LONG: rsi14 must be below this
    double RSI_OVERBOUGHT   = 65.0;   // SHORT: rsi14 must be above this

    // ---- Geometry (tunable via engine_init.hpp) ------------------------------
    // TP is the bb_mid at entry (passed in via on_tick) -- no TP_PTS param.
    // SL_PTS is the distance from entry to stop. Tight by design: if a BB
    // extreme touch + RSI extreme doesn't snap back within ~0.40pt of
    // additional adverse, the structure is broken.
    double SL_PTS           = 0.40;
    double COST_COVER_MULT  = 1.0;    // cost-gate coverage multiplier

    // ---- BE lock + trail (asymmetric payoff -- proven design from QSC) -------
    // BE arms the moment cost is covered; trail tight thereafter. Once
    // armed, worst-case outcome on the trade is BE_SCRATCH (+/- buffer).
    double BE_ARM_PTS       = 0.30;   // MFE threshold to arm BE lock
    double BE_BUFFER_PTS    = 0.05;   // ratchet SL to entry +/- this
    double TRAIL_TIGHT_PTS  = 0.15;   // trail distance after BE arm

    // ---- Filters (tunable via engine_init.hpp) -------------------------------
    double ATR_FLOOR_M1     = 0.50;   // skip dead-tape entries
    double ATR_CAP_M1       = 8.00;   // skip news-spike entries
    double SPREAD_CAP_PTS   = 0.40;   // skip wide-spread bars
    int    MAX_HOLD_SEC     = 600;    // 10-min mean-revert window
    int    COOLDOWN_SEC     = 60;     // gap between trades

    // ---- Sizing --------------------------------------------------------------
    static constexpr double USD_PER_PT_LOT = 100.0;  // XAUUSD
    double LOT_BASE         = 0.01;

    // ---- Public state --------------------------------------------------------
    bool shadow_mode = true;
    bool enabled     = true;

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;
    CloseCallback on_close_cb;

    // ---- Position tracking ---------------------------------------------------
    struct LivePos {
        bool    active     = false;
        bool    is_long    = false;
        double  entry      = 0.0;
        double  hard_sl    = 0.0;   // initial SL
        double  trail_sl   = 0.0;   // ratcheting SL (= effective SL after BE arm)
        double  tp         = 0.0;   // bb_mid at entry
        double  mfe        = 0.0;
        double  mfe_price  = 0.0;
        double  mae        = 0.0;
        bool    be_armed   = false;
        int64_t entry_ts   = 0;
        double  atr_at_entry    = 0.0;
        double  spread_at_entry = 0.0;
        double  bb_upper_at_entry = 0.0;
        double  bb_lower_at_entry = 0.0;
        double  rsi_at_entry      = 0.0;
        double  size       = 0.01;
    } m_pos;

    bool has_open_position() const noexcept { return m_pos.active; }

    // =========================================================================
    // prime_from_atomics: no-op diagnostic.
    //
    // BBandScalpEngine has no internal indicator state -- bb_upper/mid/lower,
    // rsi14, and atr14 are sourced from g_bars_gold.m1.ind atomics on every
    // tick via the dispatch in tick_gold.hpp. This method exists for
    // symmetry with GSP / TrendPullback startup wiring; it just emits a
    // diagnostic so the startup log confirms the engine sees live atomic
    // values.
    // =========================================================================
    void prime_from_atomics(double bb_upper, double bb_mid, double bb_lower,
                            double rsi14, double atr14) noexcept {
        std::printf("[BBS-PRIME] bb_upper=%.2f bb_mid=%.2f bb_lower=%.2f "
                    "rsi14=%.1f atr14=%.2f -- engine reads atomics each tick, "
                    "no warmup needed\n",
                    bb_upper, bb_mid, bb_lower, rsi14, atr14);
        std::fflush(stdout);
    }

    // =========================================================================
    // Public interface: feed every XAUUSD tick.
    //
    // bb_upper / bb_mid / bb_lower : current M1 Bollinger band values
    //   (g_bars_gold.m1.ind.bb_upper / bb_mid / bb_lower atomics in prod)
    // rsi14_m1 / atr14_m1          : current M1 Wilder indicators
    // l2_imbalance / l2_real       : optional, not required for entry
    // =========================================================================
    void on_tick(double bid, double ask, int64_t now_ms,
                 bool   can_enter,
                 double bb_upper, double bb_mid, double bb_lower,
                 double rsi14_m1, double atr14_m1,
                 double l2_imbalance = 0.5, bool l2_real = false)
    {
        if (!enabled) return;

        const double spread = ask - bid;

        // ----- Manage open position -----
        if (m_pos.active) {
            _manage(bid, ask, now_ms);
            return;
        }

        // ----- Entry gates (cheap rejects first) -----
        if (!can_enter) return;
        if (now_ms < m_cooldown_until) return;
        if (!_is_session_active(now_ms)) return;
        if (_is_weekend(now_ms)) return;
        if (spread > SPREAD_CAP_PTS) return;

        // Indicator readiness: any zero/uninitialised value means M1 bars
        // haven't completed the BB_P warmup yet. Conservative skip.
        if (bb_upper <= 0.0 || bb_mid <= 0.0 || bb_lower <= 0.0) return;
        if (bb_upper <= bb_lower) return;  // collapsed band -- skip
        if (rsi14_m1 <= 0.0 || rsi14_m1 >= 100.0) return;
        if (atr14_m1 < ATR_FLOOR_M1 || atr14_m1 > ATR_CAP_M1) return;

        // ----- Signal evaluation (mean-reversion at band extremes) -----
        // LONG: bid has touched/pierced the LOWER band AND rsi is oversold.
        //       We want to buy AT the extreme, not after it has recovered.
        // SHORT: ask has touched/pierced the UPPER band AND rsi is overbought.
        const bool long_signal  = (bid <= bb_lower) && (rsi14_m1 < RSI_OVERSOLD);
        const bool short_signal = (ask >= bb_upper) && (rsi14_m1 > RSI_OVERBOUGHT);

        if (!long_signal && !short_signal) return;

        // Both signals simultaneously should not happen on a sane tape but
        // if they do, no-op (band is collapsed or data is corrupt).
        if (long_signal && short_signal) return;

        const bool is_long = long_signal;

        // ----- Geometry: TP = bb_mid, SL = entry +/- SL_PTS -----
        const double entry = is_long ? ask : bid;
        const double sl    = is_long ? (entry - SL_PTS) : (entry + SL_PTS);
        const double tp    = bb_mid;

        // TP must be on the right side of entry (sanity check -- if mid is
        // already past entry against us, the band signal is degenerate).
        const double tp_dist = is_long ? (tp - entry) : (entry - tp);
        if (tp_dist <= 0.0) return;

        // ----- Cost gate -----
        if (!::ExecutionCostGuard::is_viable("XAUUSD", spread, tp_dist,
                                             LOT_BASE, COST_COVER_MULT)) return;

        // ----- Open position -----
        m_pos                     = LivePos{};
        m_pos.active              = true;
        m_pos.is_long             = is_long;
        m_pos.entry               = entry;
        m_pos.hard_sl             = sl;
        m_pos.trail_sl            = sl;
        m_pos.tp                  = tp;
        m_pos.mfe                 = 0.0;
        m_pos.mfe_price           = entry;
        m_pos.mae                 = 0.0;
        m_pos.be_armed            = false;
        m_pos.entry_ts            = now_ms;
        m_pos.atr_at_entry        = atr14_m1;
        m_pos.spread_at_entry     = spread;
        m_pos.bb_upper_at_entry   = bb_upper;
        m_pos.bb_lower_at_entry   = bb_lower;
        m_pos.rsi_at_entry        = rsi14_m1;
        m_pos.size                = LOT_BASE;

        char buf[480];
        std::snprintf(buf, sizeof(buf),
            "[BBS] OPEN %s entry=%.2f sl=%.2f tp=%.2f(bb_mid) size=%.3f "
            "bb_l=%.2f bb_u=%.2f rsi=%.1f atr=%.2f spread=%.2f "
            "l2_imb=%.3f l2=%s shadow=%s\n",
            is_long ? "LONG" : "SHORT",
            entry, sl, tp, LOT_BASE,
            bb_lower, bb_upper, rsi14_m1, atr14_m1, spread,
            l2_imbalance, l2_real ? "live" : "stale",
            shadow_mode ? "true" : "false");
        std::printf("%s", buf);
        std::fflush(stdout);
    }

private:
    // ---- Cooldown ----
    int64_t m_cooldown_until = 0;

    // =========================================================================
    // Manage open position: BE lock + trail + SL/TP/time exits
    // =========================================================================
    void _manage(double bid, double ask, int64_t now_ms) {
        if (!m_pos.active) return;

        const double move = m_pos.is_long
                          ? (bid - m_pos.entry)
                          : (m_pos.entry - ask);
        const double adverse = -move;

        // Update MFE/MAE
        if (move > m_pos.mfe) {
            m_pos.mfe       = move;
            m_pos.mfe_price = m_pos.is_long ? bid : ask;
        }
        if (adverse > m_pos.mae) m_pos.mae = adverse;

        // ---- BE arm: lock SL at entry +/- buffer once MFE >= BE_ARM_PTS ----
        if (!m_pos.be_armed && m_pos.mfe >= BE_ARM_PTS) {
            const double be_sl = m_pos.is_long
                ? (m_pos.entry + BE_BUFFER_PTS)
                : (m_pos.entry - BE_BUFFER_PTS);
            // Ratchet: only move in favorable direction (cannot retreat)
            if (m_pos.is_long) m_pos.trail_sl = std::max(m_pos.trail_sl, be_sl);
            else               m_pos.trail_sl = std::min(m_pos.trail_sl, be_sl);
            m_pos.be_armed = true;

            char buf[256];
            std::snprintf(buf, sizeof(buf),
                "[BBS] BE_ARMED %s entry=%.2f mfe=%.2f new_sl=%.2f\n",
                m_pos.is_long ? "LONG" : "SHORT",
                m_pos.entry, m_pos.mfe, m_pos.trail_sl);
            std::printf("%s", buf);
            std::fflush(stdout);
        }

        // ---- Trail tight after BE arm ----
        if (m_pos.be_armed) {
            const double trail_target = m_pos.is_long
                ? (m_pos.mfe_price - TRAIL_TIGHT_PTS)
                : (m_pos.mfe_price + TRAIL_TIGHT_PTS);
            if (m_pos.is_long) m_pos.trail_sl = std::max(m_pos.trail_sl, trail_target);
            else               m_pos.trail_sl = std::min(m_pos.trail_sl, trail_target);
        }

        // ---- Exits ----
        // Effective SL is the most favorable of hard_sl and trail_sl.
        const double eff_sl = m_pos.is_long
            ? std::max(m_pos.hard_sl, m_pos.trail_sl)
            : std::min(m_pos.hard_sl, m_pos.trail_sl);

        if (m_pos.is_long && bid <= eff_sl) {
            const char* reason = m_pos.be_armed
                ? (m_pos.trail_sl > m_pos.entry ? "TRAIL_WIN" : "BE_SCRATCH")
                : "SL_HIT";
            _close(bid, ask, now_ms, reason);
            return;
        }
        if (!m_pos.is_long && ask >= eff_sl) {
            const char* reason = m_pos.be_armed
                ? (m_pos.trail_sl < m_pos.entry ? "TRAIL_WIN" : "BE_SCRATCH")
                : "SL_HIT";
            _close(bid, ask, now_ms, reason);
            return;
        }
        if (m_pos.is_long && bid >= m_pos.tp) {
            _close(bid, ask, now_ms, "TP_HIT");
            return;
        }
        if (!m_pos.is_long && ask <= m_pos.tp) {
            _close(bid, ask, now_ms, "TP_HIT");
            return;
        }

        // Time stop
        if (now_ms - m_pos.entry_ts >= (int64_t)MAX_HOLD_SEC * 1000LL) {
            _close(bid, ask, now_ms, "TIME_STOP");
            return;
        }
    }

    // =========================================================================
    // Close position, emit TradeRecord, set cooldown
    //
    // S99h fix (2026-05-18 part C): tr.pnl/mfe/mae are reported in
    // points*lots (NOT USD). trade_lifecycle.hpp:218-224 multiplies by
    // tick_value_multiplier(tr.symbol) = 100 for XAUUSD downstream.
    // See GoldScalpPyramidEngine.hpp _close_position for the rationale
    // and XauThreeBar30mEngine.hpp for the reference convention.
    // =========================================================================
    void _close(double bid, double ask, int64_t now_ms, const char* reason) {
        const double exit_px = m_pos.is_long ? bid : ask;
        const double pnl_pts = m_pos.is_long
            ? (exit_px - m_pos.entry)
            : (m_pos.entry - exit_px);
        // pnl_pts_lots goes into tr.pnl (downstream multiplies to USD).
        // pnl_usd is kept ONLY for the human-readable stdout log below.
        const double pnl_pts_lots = pnl_pts * m_pos.size;
        const double pnl_usd      = pnl_pts_lots * USD_PER_PT_LOT;

        char buf[512];
        std::snprintf(buf, sizeof(buf),
            "[BBS] CLOSE %s entry=%.2f exit=%.2f pnl=$%.2f size=%.3f "
            "mfe=%.2f mae=%.2f be_armed=%d reason=%s shadow=%s\n",
            m_pos.is_long ? "LONG" : "SHORT",
            m_pos.entry, exit_px, pnl_usd, m_pos.size,
            m_pos.mfe, m_pos.mae, (int)m_pos.be_armed, reason,
            shadow_mode ? "true" : "false");
        std::printf("%s", buf);
        std::fflush(stdout);

        omega::TradeRecord tr;
        tr.engine     = "BBandScalp";
        tr.symbol     = "XAUUSD";
        tr.side       = m_pos.is_long ? "LONG" : "SHORT";
        tr.regime     = "BB_RSI_MEANREV";
        tr.entryPrice = m_pos.entry;
        tr.exitPrice  = exit_px;
        tr.tp         = m_pos.tp;
        tr.sl         = m_pos.hard_sl;
        tr.size       = m_pos.size;
        tr.pnl        = pnl_pts_lots;                  // pts*lots; downstream mults to USD
        tr.entryTs    = m_pos.entry_ts / 1000LL;
        tr.exitTs     = now_ms / 1000LL;
        tr.exitReason = reason;
        tr.mfe        = m_pos.mfe * m_pos.size;        // pts*lots
        tr.mae        = m_pos.mae * m_pos.size;        // pts*lots
        tr.spreadAtEntry = m_pos.spread_at_entry;
        tr.atr_at_entry  = m_pos.atr_at_entry;
        tr.shadow     = shadow_mode;

        if (on_close_cb) on_close_cb(tr);

        m_pos = LivePos{};
        m_cooldown_until = now_ms + (int64_t)COOLDOWN_SEC * 1000LL;
    }

    // ---- Time helpers ----
    static bool _is_weekend(int64_t ts_ms) noexcept {
        const int64_t s   = ts_ms / 1000LL;
        const int     dow = static_cast<int>((s / 86400LL + 3) % 7);
        const int     hr  = static_cast<int>((s % 86400LL) / 3600LL);
        if (dow == 4 && hr >= 20) return true;
        if (dow == 5) return true;
        if (dow == 6 && hr < 22) return true;
        return false;
    }

    static bool _is_session_active(int64_t ts_ms) noexcept {
        const int64_t s  = ts_ms / 1000LL;
        const int     hr = static_cast<int>((s % 86400LL) / 3600LL);
        return (hr >= 7 && hr < 21);  // London + NY
    }
};

}  // namespace omega
