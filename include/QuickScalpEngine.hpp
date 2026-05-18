#pragma once
// =============================================================================
// QuickScalpEngine.hpp -- Tick-driven RSI + L2 scalper with hard BE lock
// =============================================================================
//
// 2026-05-18 SESSION DESIGN (Operator request):
//   Catch "obvious" small directional moves that bar-based engines miss.
//   Asymmetric payoff: cover cost, lock BE, trail tight -- once past BE the
//   worst outcome is scratch. Many small wins + occasional scratch =
//   positive expectancy IF the entry filter has any directional edge.
//
// ENTRY (momentum continuation with multi-factor confirmation):
//   LONG:
//     - 20-tick smoothed L2 imbalance > L2_IMBAL_LONG_MIN (default 0.62)
//     - 20-tick smoothed book_slope > L2_SLOPE_CONFIRM   (default 0.15)
//     - price velocity over last VELOCITY_WINDOW_SEC > +VELOCITY_PTS
//     - M1 RSI14 in [RSI_LONG_FLOOR, RSI_LONG_CEIL] (default 50..70)
//     - l2_real==true (no live DOM -> no entry; backtest gates separately)
//   SHORT: same shape inverted
//
// COST COVERAGE:
//   TP_DIST = max(TP_ATR_MULT * M1_ATR, TP_MIN_PTS)
//   Pre-entry: ExecutionCostGuard::is_viable("XAUUSD", spread, TP_DIST,
//              LOT_BASE, COST_COVER_MULT) must return true.
//
// THE "CANNOT LOSE" PROMISE (operator requirement):
//   Phase 1 (initial):     SL = entry +/- SL_PTS (default 1.0 pts)
//   Phase 2 (BE arm):      when MFE >= BE_ARM_PTS (default 0.80 pts =
//                          cost + slip + margin), SL ratchets to
//                          entry +/- BE_BUFFER_PTS (default 0.10).
//                          Once armed, m_be_armed=true and SL never
//                          retreats below this level.
//   Phase 3 (Tight trail): after BE arm, every higher MFE pulls trail
//                          to mfe_price -/+ TRAIL_TIGHT_PTS (default 0.30).
//   Phase 4 (Time stop):   close at MAX_HOLD_SEC if neither SL nor TP hit.
//
// FAIL-LOUD INVARIANT: after BE arm, SL is monotonic in the favorable
//   direction. Verified via std::max/min ratchet in _manage().
//
// L2 SMOOTHING (per 2026-05-18 live log evidence of noisy raw imbalance):
//   L2_AVG_WINDOW ticks of rolling average over both imbalance and slope.
//   Single-tick spikes do not trigger; sustained pressure does.
//
// SIZING:
//   Fixed LOT_BASE per trade (default 0.01). Conservative scalping
//   approach -- many small bets, no pyramid, no risk-pct sizing.
//
// SESSION GATE:
//   07:00-21:00 UTC (London + NY only). Same window as GoldScalpPyramid.
//   No Asia, no weekend.
//
// L2 FIELDS (from MacroContext via tick_gold dispatch):
//   l2_imbalance, book_slope, l2_real.
//   Falls through to no-entry when l2_real=false (live only requirement).
//
// RSI / ATR SOURCING:
//   rsi14_m1 and atr14_m1 sourced from g_bars_gold.m1.ind atomics by the
//   dispatch layer in tick_gold.hpp. Backtest harness computes both
//   inline from the synthesized M1 bar stream.
//
// LOG NAMESPACE:
//   [QSC] prefix on all log lines.
//   tr.engine = "QuickScalp", tr.regime = "L2_RSI_MOM".
//
// SAFETY:
//   shadow_mode = true by default. Production engine, promote after
//   sweep validation on fresh tape (backtest/quick_scalp_bt.cpp).
//   Respects gold_any_open mutual exclusion via has_open_position()
//   contribution at the tick_gold dispatch layer.
//
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

class QuickScalpEngine {
public:
    // ---- L2 filter (tunable via engine_init.hpp) -----------------------------
    double L2_IMBAL_LONG_MIN  = 0.62;   // smoothed imb > this for long
    double L2_IMBAL_SHORT_MAX = 0.38;   // smoothed imb < this for short
    double L2_SLOPE_CONFIRM   = 0.15;   // |smoothed slope| > this required
    int    L2_AVG_WINDOW      = 20;     // rolling N-tick smoothing

    // ---- Price velocity filter -----------------------------------------------
    int    VELOCITY_WINDOW_SEC = 20;    // measure velocity over N seconds
    double VELOCITY_PTS        = 0.80;  // min directional move in window (pts)

    // ---- RSI filter (NOT too extreme - avoid late entries) -------------------
    double RSI_LONG_FLOOR   = 50.0;   // RSI must be ABOVE this for long
    double RSI_LONG_CEIL    = 70.0;   // RSI must be BELOW this for long
    double RSI_SHORT_FLOOR  = 30.0;   // RSI must be ABOVE this for short
    double RSI_SHORT_CEIL   = 50.0;   // RSI must be BELOW this for short

    // ---- Geometry ------------------------------------------------------------
    double TP_ATR_MULT      = 1.20;   // TP = max(TP_ATR_MULT * ATR, TP_MIN_PTS)
    double TP_MIN_PTS       = 1.50;   // hard floor on TP distance
    double SL_PTS           = 1.00;   // initial SL distance (pts)
    double COST_COVER_MULT  = 1.0;    // cost-gate coverage multiplier

    // ---- BE lock + trail (THE asymmetric payoff knobs) -----------------------
    double BE_ARM_PTS       = 0.80;   // MFE >= this arms BE lock
    double BE_BUFFER_PTS    = 0.10;   // SL parked at entry +/- this after BE
    double TRAIL_TIGHT_PTS  = 0.30;   // trail this far behind MFE after BE arm

    // ---- Filters -------------------------------------------------------------
    double ATR_FLOOR_M1     = 1.00;   // M1 ATR floor (no dead tape)
    double ATR_CAP_M1       = 8.00;   // M1 ATR cap (no news spikes)
    double SPREAD_CAP_PTS   = 0.60;
    int    MAX_HOLD_SEC     = 600;    // 10 min time stop
    int    COOLDOWN_SEC     = 90;     // between fires

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
        double  tp         = 0.0;
        double  mfe        = 0.0;
        double  mfe_price  = 0.0;
        double  mae        = 0.0;
        bool    be_armed   = false;
        int64_t entry_ts   = 0;
        double  atr_at_entry = 0.0;
        double  spread_at_entry = 0.0;
        double  size       = 0.01;
    } m_pos;

    bool has_open_position() const noexcept { return m_pos.active; }

    // -------------------------------------------------------------------------
    // Public interface: feed every XAUUSD tick.
    // L2 fields from MacroContext (l2_imbalance, book_slope, l2_real).
    // rsi14_m1 / atr14_m1 from g_bars_gold.m1.ind atomics (caller side).
    // -------------------------------------------------------------------------
    void on_tick(double bid, double ask, int64_t now_ms,
                 bool   can_enter,
                 double l2_imbalance, double book_slope, bool l2_real,
                 double rsi14_m1, double atr14_m1)
    {
        if (!enabled) return;

        const double mid    = (bid + ask) * 0.5;
        const double spread = ask - bid;

        // Maintain price-velocity ring buffer (mid, timestamp)
        m_price_history.push_back({now_ms, mid});
        while (!m_price_history.empty()
               && m_price_history.front().ts_ms < now_ms - (int64_t)VELOCITY_WINDOW_SEC * 1000LL) {
            m_price_history.pop_front();
        }

        // Maintain L2 smoothing buffers
        if (l2_real) {
            m_l2_imb_buf.push_back(l2_imbalance);
            m_l2_slope_buf.push_back(book_slope);
            if ((int)m_l2_imb_buf.size()   > L2_AVG_WINDOW) m_l2_imb_buf.pop_front();
            if ((int)m_l2_slope_buf.size() > L2_AVG_WINDOW) m_l2_slope_buf.pop_front();
        } else {
            // L2 stale: drop buffers so a re-armed fire requires fresh evidence
            if (!m_l2_imb_buf.empty())   m_l2_imb_buf.clear();
            if (!m_l2_slope_buf.empty()) m_l2_slope_buf.clear();
        }

        // ----- Manage open position -----
        if (m_pos.active) {
            _manage(bid, ask, now_ms);
            return;
        }

        // ----- Entry gates -----
        if (!can_enter) return;
        if (now_ms < m_cooldown_until) return;
        if (!_is_session_active(now_ms)) return;
        if (_is_weekend(now_ms)) return;
        if (spread > SPREAD_CAP_PTS) return;
        if (atr14_m1 < ATR_FLOOR_M1 || atr14_m1 > ATR_CAP_M1) return;
        if (!l2_real) return;  // live DOM required for this engine

        // Need full L2 smoothing window before firing
        if ((int)m_l2_imb_buf.size() < L2_AVG_WINDOW) return;
        if ((int)m_l2_slope_buf.size() < L2_AVG_WINDOW) return;
        // Need full velocity window
        if (m_price_history.size() < 2) return;
        const int64_t span_ms = now_ms - m_price_history.front().ts_ms;
        if (span_ms < (int64_t)VELOCITY_WINDOW_SEC * 1000LL - 2000LL) return;  // 2s slack

        // Compute smoothed L2
        double avg_imb   = 0.0, avg_slope = 0.0;
        for (double v : m_l2_imb_buf)   avg_imb   += v;
        for (double v : m_l2_slope_buf) avg_slope += v;
        avg_imb   /= (double)m_l2_imb_buf.size();
        avg_slope /= (double)m_l2_slope_buf.size();

        // Price velocity over window (signed, points)
        const double velocity = mid - m_price_history.front().px;

        // TP distance and cost-gate check
        const double tp_dist = std::max(TP_ATR_MULT * atr14_m1, TP_MIN_PTS);
        if (!::ExecutionCostGuard::is_viable("XAUUSD", spread, tp_dist,
                                             LOT_BASE, COST_COVER_MULT)) return;

        // ----- Signal evaluation -----
        const bool long_signal  =
              (velocity      >  VELOCITY_PTS)
           && (avg_imb       >  L2_IMBAL_LONG_MIN)
           && (avg_slope     >  L2_SLOPE_CONFIRM)
           && (rsi14_m1      >  RSI_LONG_FLOOR)
           && (rsi14_m1      <  RSI_LONG_CEIL);

        const bool short_signal =
              (velocity      < -VELOCITY_PTS)
           && (avg_imb       <  L2_IMBAL_SHORT_MAX)
           && (avg_slope     < -L2_SLOPE_CONFIRM)
           && (rsi14_m1      >  RSI_SHORT_FLOOR)
           && (rsi14_m1      <  RSI_SHORT_CEIL);

        if (!long_signal && !short_signal) return;

        const bool is_long = long_signal;  // long takes precedence (single trade)
        const double entry = is_long ? ask : bid;
        const double sl    = is_long ? (entry - SL_PTS) : (entry + SL_PTS);
        const double tp    = is_long ? (entry + tp_dist) : (entry - tp_dist);

        m_pos                 = LivePos{};
        m_pos.active          = true;
        m_pos.is_long         = is_long;
        m_pos.entry           = entry;
        m_pos.hard_sl         = sl;
        m_pos.trail_sl        = sl;
        m_pos.tp              = tp;
        m_pos.mfe             = 0.0;
        m_pos.mfe_price       = entry;
        m_pos.mae             = 0.0;
        m_pos.be_armed        = false;
        m_pos.entry_ts        = now_ms;
        m_pos.atr_at_entry    = atr14_m1;
        m_pos.spread_at_entry = spread;
        m_pos.size            = LOT_BASE;

        char buf[400];
        snprintf(buf, sizeof(buf),
            "[QSC] OPEN %s entry=%.2f sl=%.2f tp=%.2f size=%.3f "
            "vel=%.2f imb=%.3f slope=%.3f rsi=%.1f atr=%.2f spread=%.2f shadow=%s\n",
            is_long ? "LONG" : "SHORT",
            entry, sl, tp, LOT_BASE,
            velocity, avg_imb, avg_slope, rsi14_m1, atr14_m1, spread,
            shadow_mode ? "true" : "false");
        std::printf("%s", buf);
        std::fflush(stdout);
    }

private:
    // ---- Tick history for velocity ----
    struct TickPx { int64_t ts_ms; double px; };
    std::deque<TickPx> m_price_history;

    // ---- L2 smoothing buffers ----
    std::deque<double> m_l2_imb_buf;
    std::deque<double> m_l2_slope_buf;

    // ---- Cooldown ----
    int64_t m_cooldown_until = 0;

    // =========================================================================
    // Manage open position: BE lock + trail + SL/TP/time exits
    // =========================================================================
    void _manage(double bid, double ask, int64_t now_ms) {
        if (!m_pos.active) return;

        const double current = m_pos.is_long ? bid : ask;
        const double move    = m_pos.is_long
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
            snprintf(buf, sizeof(buf),
                "[QSC] BE_ARMED %s entry=%.2f mfe=%.2f new_sl=%.2f\n",
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
        // Effective SL is the most favorable of hard_sl and trail_sl
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
    // =========================================================================
    void _close(double bid, double ask, int64_t now_ms, const char* reason) {
        const double exit_px = m_pos.is_long ? bid : ask;
        const double pnl_pts = m_pos.is_long
            ? (exit_px - m_pos.entry)
            : (m_pos.entry - exit_px);
        const double pnl_usd = pnl_pts * USD_PER_PT_LOT * m_pos.size;

        char buf[512];
        snprintf(buf, sizeof(buf),
            "[QSC] CLOSE %s entry=%.2f exit=%.2f pnl=$%.2f size=%.3f "
            "mfe=%.2f mae=%.2f be_armed=%d reason=%s shadow=%s\n",
            m_pos.is_long ? "LONG" : "SHORT",
            m_pos.entry, exit_px, pnl_usd, m_pos.size,
            m_pos.mfe, m_pos.mae, (int)m_pos.be_armed, reason,
            shadow_mode ? "true" : "false");
        std::printf("%s", buf);
        std::fflush(stdout);

        omega::TradeRecord tr;
        tr.engine     = "QuickScalp";
        tr.symbol     = "XAUUSD";
        tr.side       = m_pos.is_long ? "LONG" : "SHORT";
        tr.regime     = "L2_RSI_MOM";
        tr.entryPrice = m_pos.entry;
        tr.exitPrice  = exit_px;
        tr.size       = m_pos.size;
        tr.pnl        = pnl_usd;
        tr.entryTs    = m_pos.entry_ts / 1000LL;
        tr.exitTs     = now_ms / 1000LL;
        tr.exitReason = reason;
        tr.mfe        = m_pos.mfe;
        tr.mae        = m_pos.mae;
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
