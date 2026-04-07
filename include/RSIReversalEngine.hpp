#pragma once
// =============================================================================
// RSIReversalEngine.hpp  --  Direct RSI extreme reversal entries for XAUUSD
// =============================================================================
//
// WHAT THIS SOLVES:
//   The chart shows RSI at 20, price at local low, 40-minute rally follows.
//   Nothing fired because:
//     1. MeanReversionEngine only activates in MEAN_REVERSION regime -- regime
//        governor sees prior downtrend and classifies as TREND/COMPRESSION.
//     2. RSI is not in GoldSnapshot so MeanReversionEngine can't read it anyway.
//     3. stack_can_enter_mr requires QUIET_COMPRESSION or CHOP_REVERSAL which
//        is never set during/after a sharp directional move.
//
// DESIGN: Bypass GoldEngineStack regime system entirely.
//   - Reads g_bars_gold.m1.ind.rsi14 directly (real M1 RSI, not z-score proxy)
//   - LONG when RSI < RSI_OVERSOLD (35) -- price at statistical bottom
//   - SHORT when RSI > RSI_OVERBOUGHT (65) -- price at statistical top
//   - ATR-based SL so position size scales with volatility automatically
//   - Progressive trail: BE lock at 1R, trail at 1.5R, tight at 2R
//   - Asia + London + NY -- all sessions. Dead zone (05-07 UTC) blocked.
//   - Cooldown 120s after any exit -- prevents immediate re-entry on same swing
//   - Regime confirmation OPTIONAL not required -- RSI extreme is the signal
//   - Additional confirmation: RSI must be TURNING (not still falling/rising)
//     to avoid entering mid-crash. Checks that last 3 RSI values show inflection.
//
// PROTECTION:
//   - SL = ATR * SL_ATR_MULT (default 1.0x ATR from entry)
//   - BE lock when move >= ATR * 1.0 (covers SL cost)
//   - Trail distance = ATR * TRAIL_ATR_MULT (default 0.75x ATR)
//   - Hard max hold 20 minutes -- if trade goes nowhere, exit
//   - VWAP headwind filter: don't enter LONG when price is already >8pt above VWAP
//     (momentum exhausted) and don't enter SHORT when >8pt below VWAP
//
// PARAMETERS (calibrated on Asia session):
//   RSI_OVERSOLD  = 35   -- below this = buy
//   RSI_OVERBOUGHT= 65   -- above this = sell
//   SL_ATR_MULT   = 1.0  -- SL at 1x ATR from entry
//   TRAIL_ATR_MULT= 0.75 -- trail at 0.75x ATR behind MFE
//   BE_ATR_MULT   = 1.0  -- BE locks when move >= 1x ATR
//   MAX_HOLD_S    = 1200 -- 20 minute hard exit
//   COOLDOWN_S    = 120  -- 2 minute cooldown after any close
//   RSI_CONFIRM_BARS = 2 -- need 2 ticks of RSI turning before entry
// =============================================================================

#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <ctime>
#include <functional>
#include <string>
#include <deque>
#include "OmegaTradeLedger.hpp"

namespace omega {

class RSIReversalEngine {
public:
    // ── Tunable parameters ───────────────────────────────────────────────────
    double RSI_OVERSOLD       = 38.0;  // LONG entry threshold (catches more turns)
    double RSI_OVERBOUGHT     = 62.0;  // SHORT entry threshold (catches more turns)
    double RSI_EXIT_LONG      = 55.0;  // exit LONG when RSI recovers to here (take profit)
    double RSI_EXIT_SHORT     = 45.0;  // exit SHORT when RSI recovers to here (take profit)
    double SL_ATR_MULT        = 0.6;   // SL = 0.6x ATR -- tight enough for small oscillations
                                       // Asia ATR=3pt: SL=$1.80. Commission=$0.60. Need $2.40 to profit.
                                       // RSI 38->55 typically moves $2-4pt -- covers this.
    double TRAIL_ATR_MULT     = 0.40;  // trail = 0.4x ATR -- tight trail, don't give back the move
    double BE_ATR_MULT        = 0.40;  // BE locks at 0.4x ATR -- locks very early (at spread+commission cost)
                                       // Asia ATR=3pt: BE at $1.20 move = cost covered, zero risk
    double VWAP_HEADWIND_PTS  = 12.0;  // raised 8->12: don't block valid swings near VWAP
    double MAX_SPREAD_PTS     = 2.5;   // max spread at entry
    double MIN_ATR_PTS        = 1.5;   // minimum ATR (lowered 2.0->1.5: trade quiet Asia tape too)
    int    COOLDOWN_S         = 60;    // 60s cooldown (lowered 120->60: RSI can cycle again quickly)
    int    MAX_HOLD_S         = 600;   // 10 min hard exit (lowered 20->10: RSI swings resolve faster)
    int    MIN_HOLD_S         = 8;     // 8s minimum (lowered 15->8: tight SL means quick resolution)
    int    RSI_CONFIRM_TICKS  = 1;     // 1 tick turn confirmation (lowered 2->1: enter faster on turn)
    bool   enabled            = true;
    bool   shadow_mode        = true;  // default shadow until validated

    // ── Observable state ─────────────────────────────────────────────────────
    struct Position {
        bool    active    = false;
        bool    is_long   = false;
        bool    be_locked = false;
        double  entry     = 0.0;
        double  sl        = 0.0;
        double  atr       = 0.0;
        double  size      = 0.01;
        double  mfe       = 0.0;
        int64_t entry_ts  = 0;
    } pos;

    bool has_open_position() const noexcept { return pos.active; }

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;

    // ── Main tick ─────────────────────────────────────────────────────────────
    // Called from tick_gold.hpp every XAUUSD tick.
    // rsi14:       live M1 RSI(14) from g_bars_gold.m1.ind.rsi14
    // atr14:       live M1 ATR(14) from g_bars_gold.m1.ind.atr14
    // bars_ready:  g_bars_gold.m1.ind.m1_ready (need at least 14 bars)
    // vwap:        g_gold_stack.vwap()
    // session_slot: g_macro_ctx.session_slot (0=dead, 1-5=London/NY, 6=Asia)
    void on_tick(double bid, double ask,
                 double rsi14, double atr14, bool bars_ready,
                 double vwap, int session_slot,
                 int64_t now_ms, CloseCallback on_close) noexcept
    {
        if (!enabled) return;
        if (bid <= 0.0 || ask <= 0.0) return;

        const double mid    = (bid + ask) * 0.5;
        const double spread = ask - bid;
        const int64_t now_s = now_ms / 1000;

        // ── Manage open position ─────────────────────────────────────────────
        if (pos.active) {
            _manage(bid, ask, mid, atr14, rsi14, now_s, on_close);
            return;
        }

        // ── Cooldown ─────────────────────────────────────────────────────────
        if (now_s < m_cooldown_until) return;

        // ── Entry gates ───────────────────────────────────────────────────────
        if (!bars_ready)             return;  // need seeded RSI/ATR
        if (rsi14 <= 0.0)            return;  // RSI not computed yet
        if (atr14 < MIN_ATR_PTS)     return;  // dead tape
        if (spread > MAX_SPREAD_PTS) return;  // spread too wide

        // Dead zone: 05:00-07:00 UTC -- thin liquidity
        if (_in_dead_zone()) return;

        // Session gate: must be active session (1-6), not slot 0
        if (session_slot == 0) return;

        // ── RSI extreme detection ─────────────────────────────────────────────
        const bool rsi_oversold   = (rsi14 < RSI_OVERSOLD);
        const bool rsi_overbought = (rsi14 > RSI_OVERBOUGHT);
        if (!rsi_oversold && !rsi_overbought) return;

        // ── RSI turning confirmation ──────────────────────────────────────────
        // Don't enter mid-crash. Require RSI to have stopped falling (for LONG)
        // or stopped rising (for SHORT). Check last N ticks show inflection.
        // Track RSI history
        m_rsi_history.push_back(rsi14);
        if ((int)m_rsi_history.size() > 10) m_rsi_history.pop_front();

        if ((int)m_rsi_history.size() < RSI_CONFIRM_TICKS + 1) return;

        // For LONG: need RSI to have been lower and now ticking up
        // For SHORT: need RSI to have been higher and now ticking down
        const double rsi_prev = m_rsi_history[m_rsi_history.size() - 2];
        if (rsi_oversold  && rsi14 <= rsi_prev) return;  // still falling -- wait
        if (rsi_overbought && rsi14 >= rsi_prev) return;  // still rising -- wait

        // ── VWAP headwind filter ──────────────────────────────────────────────
        // Don't enter LONG if price already far above VWAP (rally exhausted)
        // Don't enter SHORT if price already far below VWAP (crash exhausted)
        if (vwap > 0.0) {
            const double vwap_dist = mid - vwap;
            if (rsi_oversold  && vwap_dist >  VWAP_HEADWIND_PTS) return;  // LONG but price above VWAP by >8pt
            if (rsi_overbought && vwap_dist < -VWAP_HEADWIND_PTS) return;  // SHORT but price below VWAP by >8pt
        }

        // ── Entry ─────────────────────────────────────────────────────────────
        const bool is_long = rsi_oversold;
        const double entry = is_long ? ask : bid;
        const double sl_dist = std::max(atr14 * SL_ATR_MULT, spread * 3.0);
        const double sl_px   = is_long ? (entry - sl_dist) : (entry + sl_dist);

        pos.active   = true;
        pos.is_long  = is_long;
        pos.entry    = entry;
        pos.sl       = sl_px;
        pos.atr      = atr14;
        pos.size     = 0.01;  // sized externally by tick_gold.hpp after entry
        pos.mfe      = 0.0;
        pos.be_locked = false;
        pos.entry_ts = now_s;

        const char* pfx = shadow_mode ? "[RSI-REV-SHADOW]" : "[RSI-REV]";
        printf("%s %s entry=%.2f sl=%.2f(%.1fpt) rsi=%.1f atr=%.2f spread=%.2f slot=%d\n",
               pfx, is_long ? "LONG" : "SHORT",
               entry, sl_px, sl_dist, rsi14, atr14, spread, session_slot);
        fflush(stdout);
    }

    // Called by tick_gold.hpp after entry to patch the lot size
    void patch_size(double lot) noexcept {
        if (pos.active) pos.size = lot;
    }

    // Called by tick_gold.hpp to force-close (e.g. session end)
    void force_close(double bid, double ask, int64_t now_ms,
                     CloseCallback on_close) noexcept {
        if (!pos.active) return;
        const int64_t now_s = now_ms / 1000;
        const double exit_px = pos.is_long ? bid : ask;
        _close(exit_px, "FORCE_CLOSE", now_s, on_close);
    }

private:
    int64_t         m_cooldown_until = 0;
    int             m_trade_id       = 0;
    std::deque<double> m_rsi_history;

    static bool _in_dead_zone() noexcept {
        const auto t = std::time(nullptr);
        struct tm ti{};
#ifdef _WIN32
        gmtime_s(&ti, &t);
#else
        gmtime_r(&t, &ti);
#endif
        const int h = ti.tm_hour;
        return (h >= 5 && h < 7);  // 05:00-07:00 UTC
    }

    void _manage(double bid, double ask, double mid,
                 double atr14, double rsi14, int64_t now_s,
                 CloseCallback on_close) noexcept
    {
        if (!pos.active) return;

        const double move      = pos.is_long ? (mid - pos.entry) : (pos.entry - mid);
        const double atr_live  = (atr14 > 0.0)
            ? (0.7 * pos.atr + 0.3 * atr14)
            : pos.atr;

        if (move > pos.mfe) pos.mfe = move;

        // Minimum hold -- avoid immediate SL on spread noise
        if ((now_s - pos.entry_ts) < MIN_HOLD_S) return;

        // ?? RSI exit -- "take profit every time RSI turns back" ??????????????
        // Exit when RSI returns to neutral zone -- the mean reversion is complete.
        // Does NOT require BE to be locked first:
        //   - If RSI returns to 55/45 with us in profit: great, take it.
        //   - If RSI returns to 55/45 with us at break-even: still exit, thesis played out.
        //   - If RSI returns to 55/45 while we're losing: SL will have fired already
        //     (SL is tighter than the RSI swing so it protects against bad entries).
        // Min hold prevents exiting on the first tick after entry.
        if (rsi14 > 0.0) {
            const bool rsi_exit_long  = pos.is_long  && (rsi14 >= RSI_EXIT_LONG);
            const bool rsi_exit_short = !pos.is_long && (rsi14 <= RSI_EXIT_SHORT);
            if (rsi_exit_long || rsi_exit_short) {
                const double exit_px   = pos.is_long ? bid : ask;
                const double exit_move = pos.is_long
                    ? (exit_px - pos.entry) : (pos.entry - exit_px);
                // Only exit if we have at least break-even (spread covered)
                // Prevents RSI_EXIT firing immediately after entry on a whipsaw
                const double min_profit_pts = (ask - bid) * 0.5;  // half spread = break-even
                if (exit_move >= -min_profit_pts) {  // at or above break-even
                    printf("[RSI-REV] RSI_EXIT %s rsi=%.1f threshold=%.0f move=%.2f -- taking profit\n",
                           pos.is_long ? "LONG" : "SHORT",
                           rsi14, pos.is_long ? RSI_EXIT_LONG : RSI_EXIT_SHORT,
                           exit_move);
                    fflush(stdout);
                    _close(exit_px, "RSI_TP", now_s, on_close);
                    return;
                }
            }
        }

        // Hard max hold
        if ((now_s - pos.entry_ts) > MAX_HOLD_S) {
            const double exit_px = pos.is_long ? bid : ask;
            printf("[RSI-REV] MAX_HOLD %s held=%llds -- exit\n",
                   pos.is_long ? "LONG" : "SHORT",
                   (long long)(now_s - pos.entry_ts));
            fflush(stdout);
            _close(exit_px, "MAX_HOLD", now_s, on_close);
            return;
        }

        const double be_threshold = pos.atr * BE_ATR_MULT;

        // BE lock: advance SL to entry when move >= 1x ATR
        if (!pos.be_locked && move >= be_threshold) {
            pos.sl       = pos.entry;
            pos.be_locked = true;
            printf("[RSI-REV] BE_LOCK %s move=%.2f sl->entry=%.2f\n",
                   pos.is_long ? "LONG" : "SHORT", move, pos.entry);
            fflush(stdout);
        }

        // Progressive trail once BE locked
        if (pos.be_locked) {
            const double trail_dist = atr_live * TRAIL_ATR_MULT;
            const double trail_sl   = pos.is_long
                ? (pos.entry + pos.mfe - trail_dist)
                : (pos.entry - pos.mfe + trail_dist);
            if (pos.is_long  && trail_sl > pos.sl) pos.sl = trail_sl;
            if (!pos.is_long && trail_sl < pos.sl) pos.sl = trail_sl;
        }

        // SL check
        const bool sl_hit = pos.is_long ? (bid <= pos.sl) : (ask >= pos.sl);
        if (!sl_hit) return;

        const double exit_px = pos.is_long ? bid : ask;
        const char* reason   = pos.be_locked
            ? (std::fabs(pos.sl - pos.entry) < 0.01 ? "BE_HIT" : "TRAIL_HIT")
            : "SL_HIT";
        _close(exit_px, reason, now_s, on_close);
    }

    void _close(double exit_px, const char* reason,
                int64_t now_s, CloseCallback on_close) noexcept
    {
        const double pnl = (pos.is_long
            ? (exit_px - pos.entry)
            : (pos.entry - exit_px)) * pos.size;

        printf("[RSI-REV] EXIT %s @ %.2f reason=%s pnl_raw=%.4f mfe=%.2f\n",
               pos.is_long ? "LONG" : "SHORT",
               exit_px, reason, pnl, pos.mfe);
        fflush(stdout);

        omega::TradeRecord tr;
        tr.id          = ++m_trade_id;
        tr.symbol      = "XAUUSD";
        tr.side        = pos.is_long ? "LONG" : "SHORT";
        tr.engine      = "RSIReversal";
        tr.regime      = "RSI_REVERSAL";
        tr.entryPrice  = pos.entry;
        tr.exitPrice   = exit_px;
        tr.sl          = pos.sl;
        tr.size        = pos.size;
        tr.pnl         = pnl;
        tr.mfe         = pos.mfe * pos.size;
        tr.mae         = 0.0;
        tr.entryTs     = pos.entry_ts;
        tr.exitTs      = now_s;
        tr.exitReason  = reason;
        tr.spreadAtEntry = 0.0;

        pos = Position{};
        m_cooldown_until = now_s + COOLDOWN_S;
        m_rsi_history.clear();  // reset RSI history after close

        if (on_close) on_close(tr);
    }
};

} // namespace omega
