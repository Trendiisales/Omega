#pragma once
// =============================================================================
// GoldRegimeDailyEngine.hpp -- M30 EMA-cross entry, trend-flip primary exit
// =============================================================================
//
// 2026-05-19 PART-D SESSION DESIGN:
//
//   Per S102 part-C verdict: Donchian-break entries on M5/M15/H1 lack
//   directional edge. H1 trend-flip exit fires 0 times because the
//   BE-lock catches all retracements first.
//
//   New approach matched literally to user goal text:
//     "trade when costs are covered $0.5, take that as breakeven,
//      then allow trade, lock in profit with tight trailing and
//      exit when signal/trend reverses"
//
//   Mechanism:
//     1. Entry  -- ENTER on EMA9-cross-EMA21 events (the "signal"):
//                  long when ema9 crosses above ema21,
//                  short when ema9 crosses below ema21.
//                  This is a trend-following entry that catches the
//                  start of trend. The "signal" IS the EMA crossover.
//     2. BE arm -- once MFE >= 0.5 pts, SL ratchets to entry+BE_BUFFER.
//                  Costs covered, never lose from here.
//     3. Tight trail -- after BE arm, SL ratchets to mfe_price-TRAIL_DIST.
//                       Locks in profit as trade develops.
//     4. Exit on signal reversal -- when EMA9 crosses BACK against
//                       position direction. The signal that got us
//                       in has now reversed -> exit.
//     5. Backstops: hard SL at 1.5*ATR, hard TP at 4.0*ATR (let big
//                   winners run), time-stop at 24 bars (12h on M30).
//
//   Why M30 over H1: H1 EMA-cross fires too rarely (~few per week on
//   gold). M30 fires ~5-15x/day, providing enough trades to evaluate
//   edge while keeping bars long enough to filter tick noise.
//
//   Why no Donchian filter: S102 proved Donchian-break entries on
//   gold have no directional edge at fixed-RR. Pure EMA-cross tests
//   whether momentum-aligned entries have any edge.
//
// LOG NAMESPACE: [GRD]. tr.engine = "GoldRegimeDaily". tr.regime = "D1_REGIME".
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

class GoldRegimeDailyEngine {
public:
    static constexpr int BAR_SECS = 14400;  // H4 (4-hour bars)

    double SL_ATR_MULT  = 2.0;     // wider SL for H4 magnitudes
    double TP_ATR_MULT  = 8.0;     // very wide TP -- ride macro trends

    double COST_COVER_PTS = 5.00;  // scaled for H4 magnitudes (was 0.50 on M5)
    double BE_BUFFER_PTS  = 1.00;
    double TRAIL_DIST     = 15.0;  // wide trail -- let H4 winners run

    // Gate on trend-flip exit: only fire after BE armed OR adverse > GATE*SL
    double REVERSAL_ADVERSE_GATE = 0.50;

    static constexpr double USD_PER_PT_LOT = 100.0;
    static constexpr double RISK_DOLLARS   = 50.0;
    static constexpr double LOT_MIN        = 0.01;
    static constexpr double LOT_MAX        = 0.10;       // larger cap for big-trade regime

    static constexpr double ATR_FLOOR     = 10.0;     // H4 ATR much larger
    static constexpr double ATR_CAP       = 80.0;
    static constexpr double SPREAD_CAP    = 1.00;
    static constexpr int    MAX_HOLD_BARS = 100;      // 100 H4 = ~16 days, ride trends
    static constexpr int    COOLDOWN_SEC  = 14400;    // 4h cooldown

    bool shadow_mode = true;
    bool enabled     = true;

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;
    CloseCallback on_close_cb;

    struct LivePos {
        bool    active=false, is_long=false, be_armed=false;
        double  entry=0, hard_sl=0, hard_tp=0, trail_sl=0;
        double  mfe_peak=0, mfe_price=0, mae=0;
        double  atr_at_entry=0, spread_at_entry=0, size=0;
        int64_t entry_ts=0, entry_bar_seq=0;
    } m_pos;

    bool has_open_position() const noexcept { return m_pos.active; }

    struct BarAccum { double open=0,high=0,low=0,close=0; int64_t ts_open=0; int n=0; };

    void on_tick(double bid, double ask, int64_t now_ms,
                 bool can_enter,
                 double, double, bool, bool, bool, bool, bool,
                 const CloseCallback* ext_close = nullptr)
    {
        if (!enabled) return;
        const double mid = (bid+ask)*0.5;
        const double spread = ask-bid;

        const int64_t bar_ms = BAR_SECS*1000LL;
        const int64_t a = (now_ms/bar_ms)*bar_ms;
        if (m_cur_anchor < 0) { m_cur_bar = BarAccum{mid,mid,mid,mid,a,1}; m_cur_anchor=a; }
        else if (a != m_cur_anchor) {
            _on_bar_close(m_cur_bar);
            m_cur_bar = BarAccum{mid,mid,mid,mid,a,1}; m_cur_anchor=a; ++m_bars_seen;
        } else {
            if (mid > m_cur_bar.high) m_cur_bar.high = mid;
            if (mid < m_cur_bar.low)  m_cur_bar.low  = mid;
            m_cur_bar.close = mid; ++m_cur_bar.n;
        }

        if (m_pos.active) { _manage(bid, ask, now_ms, ext_close); return; }

        if (!m_signal_pending) return;
        m_signal_pending = false;
        if (!can_enter) return;
        if (now_ms < m_cooldown_until) return;
        if (spread > SPREAD_CAP) return;

        const double sl_dist = m_signal_atr * SL_ATR_MULT;
        const double tp_dist = m_signal_atr * TP_ATR_MULT;
        if (!::ExecutionCostGuard::is_viable("XAUUSD", spread, tp_dist, LOT_MIN, 1.5)) return;

        const double entry_px = m_signal_long ? ask : bid;
        const double sl_px    = m_signal_long ? (entry_px - sl_dist) : (entry_px + sl_dist);
        const double tp_px    = m_signal_long ? (entry_px + tp_dist) : (entry_px - tp_dist);

        double size = RISK_DOLLARS / (sl_dist * USD_PER_PT_LOT);
        size = std::floor(size/0.01)*0.01;
        size = std::max(LOT_MIN, std::min(LOT_MAX, size));

        m_pos = LivePos{};
        m_pos.active=true; m_pos.is_long=m_signal_long;
        m_pos.entry=entry_px; m_pos.hard_sl=sl_px; m_pos.hard_tp=tp_px; m_pos.trail_sl=sl_px;
        m_pos.mfe_price=entry_px; m_pos.atr_at_entry=m_signal_atr; m_pos.spread_at_entry=spread;
        m_pos.size=size; m_pos.entry_ts=now_ms; m_pos.entry_bar_seq=m_bars_seen;

        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "[GRD] OPEN %s entry=%.2f sl=%.2f tp=%.2f size=%.3f atr=%.2f ema9=%.2f ema21=%.2f\n",
            m_signal_long?"LONG":"SHORT", entry_px, sl_px, tp_px, size, m_signal_atr,
            m_ema9.value, m_ema21.value);
        std::printf("%s", buf); std::fflush(stdout);
    }

private:
    BarAccum m_cur_bar{};
    int64_t  m_cur_anchor = -1;
    int64_t  m_bars_seen  = 0;

    struct EMAState {
        int period=0; double value=0, alpha=0; int count=0; bool primed=false;
        void init(int p) { period=p; alpha=2.0/(p+1.0); value=0; count=0; primed=false; }
        void push(double v) {
            if (!primed) { value+=v; ++count; if (count>=period) { value/=period; primed=true; } }
            else value = alpha*v + (1.0-alpha)*value;
        }
    };
    EMAState m_ema9, m_ema21;
    bool m_ema_inited=false;
    double m_prev_ema9_minus_21 = 0.0;  // sign of (ema9 - ema21) at last bar close
    bool   m_prev_diff_set = false;

    struct ATRState {
        double value=0; bool primed=false;
        double prev_close=0; bool have_prev=false;
        std::deque<double> seed;
        void push(double h, double l, double c) {
            double tr;
            if (!have_prev) tr = h-l;
            else { double a=h-l, b=std::fabs(h-prev_close), cc=std::fabs(l-prev_close); tr=std::max(a,std::max(b,cc)); }
            have_prev=true; prev_close=c;
            if (!primed) { seed.push_back(tr); if ((int)seed.size()>=14) { double s=0; for(auto v:seed) s+=v; value=s/14.0; primed=true; } }
            else value = (value*13.0+tr)/14.0;
        }
    } m_atr;

    bool   m_signal_pending=false, m_signal_long=false;
    double m_signal_atr=0;
    int64_t m_cooldown_until=0;

    void _on_bar_close(const BarAccum& bar) {
        if (!m_ema_inited) { m_ema9.init(9); m_ema21.init(21); m_ema_inited=true; }
        m_ema9.push(bar.close);
        m_ema21.push(bar.close);
        m_atr.push(bar.high, bar.low, bar.close);

        m_signal_pending=false;
        if (!m_atr.primed || !m_ema9.primed || !m_ema21.primed) return;
        if (m_pos.active) return;
        if (_is_weekend(bar.ts_open)) return;
        if (!_is_session_active(bar.ts_open)) return;
        if (m_atr.value < ATR_FLOOR || m_atr.value > ATR_CAP) return;

        // EMA-CROSS DETECTION (the entry signal)
        // Track sign of (ema9 - ema21). Cross occurs when sign flips.
        const double diff = m_ema9.value - m_ema21.value;
        if (!m_prev_diff_set) {
            m_prev_ema9_minus_21 = diff;
            m_prev_diff_set = true;
            return;  // need 2 closes to detect a cross
        }

        // Bullish cross: prev <=0 and now >0
        const bool bull_cross = (m_prev_ema9_minus_21 <= 0.0 && diff > 0.0);
        // Bearish cross: prev >=0 and now <0
        const bool bear_cross = (m_prev_ema9_minus_21 >= 0.0 && diff < 0.0);

        m_prev_ema9_minus_21 = diff;

        if (!bull_cross && !bear_cross) return;

        m_signal_pending = true;
        m_signal_long    = bull_cross;
        m_signal_atr     = m_atr.value;
    }

    void _manage(double bid, double ask, int64_t now_ms, const CloseCallback* ext_close) {
        if (!m_pos.active) return;
        const double move = m_pos.is_long ? (bid - m_pos.entry) : (m_pos.entry - ask);
        const double adverse = -move;
        if (move > m_pos.mfe_peak) { m_pos.mfe_peak = move; m_pos.mfe_price = m_pos.is_long ? bid : ask; }
        if (adverse > m_pos.mae) m_pos.mae = adverse;

        const double sl_dist = m_pos.atr_at_entry * SL_ATR_MULT;

        // BE arm at MFE >= COST_COVER_PTS
        if (!m_pos.be_armed && m_pos.mfe_peak >= COST_COVER_PTS) {
            m_pos.be_armed = true;
            const double be = m_pos.is_long ? (m_pos.entry + BE_BUFFER_PTS) : (m_pos.entry - BE_BUFFER_PTS);
            if (m_pos.is_long) m_pos.trail_sl = std::max(m_pos.trail_sl, be);
            else               m_pos.trail_sl = std::min(m_pos.trail_sl, be);
        }

        // Tight trail post-BE: ratchet at TRAIL_DIST behind MFE
        if (m_pos.be_armed) {
            const double t = m_pos.is_long ? (m_pos.mfe_price - TRAIL_DIST) : (m_pos.mfe_price + TRAIL_DIST);
            if (m_pos.is_long) m_pos.trail_sl = std::max(m_pos.trail_sl, t);
            else               m_pos.trail_sl = std::min(m_pos.trail_sl, t);
        }

        // TREND-FLIP exit: EMA9 crosses against position direction
        // (EMAs update only on bar close; check is effectively per-bar resolution)
        if (m_ema9.primed && m_ema21.primed) {
            const bool gate_open =
                m_pos.be_armed || (adverse >= sl_dist * REVERSAL_ADVERSE_GATE);
            if (gate_open) {
                const bool trend_against =
                    (m_pos.is_long  && m_ema9.value < m_ema21.value) ||
                    (!m_pos.is_long && m_ema9.value > m_ema21.value);
                if (trend_against) {
                    _close(bid, ask, now_ms, "TREND_FLIP_EXIT", ext_close);
                    m_cooldown_until = now_ms + COOLDOWN_SEC * 1000LL;
                    return;
                }
            }
        }

        const double eff_sl = m_pos.is_long ? std::max(m_pos.hard_sl, m_pos.trail_sl)
                                            : std::min(m_pos.hard_sl, m_pos.trail_sl);
        if (m_pos.is_long && bid <= eff_sl) {
            const char* r = (m_pos.trail_sl > m_pos.hard_sl) ? "TRAIL_HIT" : "SL_HIT";
            _close(bid, ask, now_ms, r, ext_close); m_cooldown_until = now_ms + COOLDOWN_SEC*1000LL; return;
        }
        if (!m_pos.is_long && ask >= eff_sl) {
            const char* r = (m_pos.trail_sl < m_pos.hard_sl) ? "TRAIL_HIT" : "SL_HIT";
            _close(bid, ask, now_ms, r, ext_close); m_cooldown_until = now_ms + COOLDOWN_SEC*1000LL; return;
        }
        if (m_pos.is_long && bid >= m_pos.hard_tp) {
            _close(bid, ask, now_ms, "TP_HIT", ext_close); m_cooldown_until = now_ms + COOLDOWN_SEC*1000LL; return;
        }
        if (!m_pos.is_long && ask <= m_pos.hard_tp) {
            _close(bid, ask, now_ms, "TP_HIT", ext_close); m_cooldown_until = now_ms + COOLDOWN_SEC*1000LL; return;
        }
        const int bars_held = (int)(m_bars_seen - m_pos.entry_bar_seq);
        if (bars_held >= MAX_HOLD_BARS) {
            _close(bid, ask, now_ms, "TIME_STOP", ext_close); m_cooldown_until = now_ms + COOLDOWN_SEC*1000LL; return;
        }
    }

    void _close(double bid, double ask, int64_t now_ms, const char* reason, const CloseCallback* ext_close) {
        const double exit_px = m_pos.is_long ? bid : ask;
        const double move = m_pos.is_long ? (exit_px - m_pos.entry) : (m_pos.entry - exit_px);
        const double pnl_pts_lots = move * m_pos.size;
        const double pnl_usd = pnl_pts_lots * USD_PER_PT_LOT;

        char buf[384];
        std::snprintf(buf, sizeof(buf),
            "[GRD] CLOSE %s entry=%.2f exit=%.2f pnl=$%.2f size=%.3f mfe=%.2f mae=%.2f be=%d bars=%d reason=%s\n",
            m_pos.is_long?"LONG":"SHORT", m_pos.entry, exit_px, pnl_usd, m_pos.size,
            m_pos.mfe_peak, m_pos.mae, (int)m_pos.be_armed,
            (int)(m_bars_seen - m_pos.entry_bar_seq), reason);
        std::printf("%s", buf); std::fflush(stdout);

        omega::TradeRecord tr;
        tr.engine="GoldRegimeDaily"; tr.symbol="XAUUSD";
        tr.side=m_pos.is_long?"LONG":"SHORT"; tr.regime="D1_REGIME";
        tr.entryPrice=m_pos.entry; tr.exitPrice=exit_px; tr.size=m_pos.size;
        tr.pnl=pnl_pts_lots; tr.entryTs=m_pos.entry_ts/1000LL; tr.exitTs=now_ms/1000LL;
        tr.exitReason=reason;
        tr.mfe=m_pos.mfe_peak*m_pos.size; tr.mae=m_pos.mae*m_pos.size;
        tr.spreadAtEntry=m_pos.spread_at_entry; tr.shadow=shadow_mode;
        if (ext_close && *ext_close) (*ext_close)(tr);
        else if (on_close_cb) on_close_cb(tr);
        m_pos = LivePos{};
    }

    static bool _is_weekend(int64_t ts_ms) {
        const int64_t s = ts_ms/1000LL;
        const int dow = static_cast<int>((s/86400LL + 3) % 7);
        const int hr  = static_cast<int>((s%86400LL)/3600LL);
        if (dow==4 && hr>=20) return true;
        if (dow==5) return true;
        if (dow==6 && hr<22) return true;
        return false;
    }
    static bool _is_session_active(int64_t ts_ms) {
        const int64_t s = ts_ms/1000LL;
        const int hr = static_cast<int>((s%86400LL)/3600LL);
        return (hr>=7 && hr<21);
    }
};

} // namespace omega
