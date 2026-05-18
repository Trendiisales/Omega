#pragma once
// =============================================================================
// GoldFixedRREngine.hpp -- M5 entry, hard SL + hard TP, no trail/no reversal
// =============================================================================
//
// 2026-05-19 SESSION DESIGN (Claude / Jo):
//   Per S101's failure-criterion verdict, this is the alternative-mechanism
//   candidate after the reversal-detect family (V1, V2, M15) failed.
//
//   The hypothesis: the user's "cover cost -> BE -> tight trail -> reversal-
//   exit" mechanism is internally sound but cannot be profitable on
//   gold-M5/M15 breakouts because winners never develop large enough
//   excursion to be locked tight. The 71% WR comes from many breakouts
//   that follow-through briefly then retrace.
//
//   Fixed-RR removes ALL the lock/reverse logic. The trade fills, sets
//   hard SL and hard TP at SL_ATR * SL_ATR_MULT and SL_dist * RR_RATIO.
//   No trail, no BE, no reversal-detect. The trade either hits TP or SL
//   or times out at MAX_HOLD_BARS.
//
//   If gold-M5 entries DO have edge but it's hidden by exit-too-early
//   logic, fixed-RR exposes it. If gold-M5 entries have NO edge at all,
//   fixed-RR will also fail and the entry shape itself is the problem.
//
// LOG NAMESPACE: [GRR]. tr.engine = "GoldFixedRR".
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

class GoldFixedRREngine {
public:
    static constexpr int BAR_SECS = 300;

    int    LOOKBACK       = 8;
    double SL_ATR_MULT    = 1.5;    // hard SL distance in ATR-units
    double RR_RATIO       = 2.0;    // TP_dist = SL_dist * RR_RATIO

    // 2026-05-19 part-B: if true, flip the signal direction (fade the
    // breakout instead of following it). Tests whether the 71% trail-WR
    // observed in V1/V2/M15 was mean-reversion-in-disguise. False = follow
    // (original); true = fade.
    bool   REVERSE_SIGNAL = false;

    // 2026-05-19 part-D: if true, only take LONG entries (skip shorts).
    // Tests whether the 2024-03..2026-04 gold bull-run (price moved from
    // ~$1900 to ~$3300, +74%) creates a directional bias that short
    // trades systematically lose to. Long-only on Donchian-up-break in
    // a bull regime could be the simplest viable mechanism.
    bool   LONG_ONLY      = false;

    static constexpr double USD_PER_PT_LOT = 100.0;
    static constexpr double RISK_DOLLARS   = 50.0;
    static constexpr double LOT_MIN        = 0.01;
    static constexpr double LOT_MAX        = 0.05;

    static constexpr double ATR_FLOOR     = 1.50;
    static constexpr double ATR_CAP       = 15.0;
    static constexpr double SPREAD_CAP    = 0.80;
    static constexpr int    MAX_HOLD_BARS = 24;  // 2h on M5 -- give TP room
    static constexpr int    COOLDOWN_SEC  = 60;

    bool shadow_mode = true;
    bool enabled     = true;

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;
    CloseCallback on_close_cb;

    struct LivePos {
        bool    active=false, is_long=false;
        double  entry=0, hard_sl=0, hard_tp=0;
        double  mfe_peak=0, mae=0;
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
        const double tp_dist = sl_dist * RR_RATIO;
        if (!::ExecutionCostGuard::is_viable("XAUUSD", spread, tp_dist, LOT_MIN, 1.5)) return;

        const double entry_px = m_signal_long ? ask : bid;
        const double sl_px    = m_signal_long ? (entry_px - sl_dist) : (entry_px + sl_dist);
        const double tp_px    = m_signal_long ? (entry_px + tp_dist) : (entry_px - tp_dist);

        double size = RISK_DOLLARS / (sl_dist * USD_PER_PT_LOT);
        size = std::floor(size/0.01)*0.01;
        size = std::max(LOT_MIN, std::min(LOT_MAX, size));

        m_pos = LivePos{};
        m_pos.active=true; m_pos.is_long=m_signal_long;
        m_pos.entry=entry_px; m_pos.hard_sl=sl_px; m_pos.hard_tp=tp_px;
        m_pos.atr_at_entry=m_signal_atr; m_pos.spread_at_entry=spread;
        m_pos.size=size; m_pos.entry_ts=now_ms; m_pos.entry_bar_seq=m_bars_seen;

        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "[GRR] OPEN %s entry=%.2f sl=%.2f tp=%.2f size=%.3f atr=%.2f rr=%.1f\n",
            m_signal_long?"LONG":"SHORT", entry_px, sl_px, tp_px, size, m_signal_atr, RR_RATIO);
        std::printf("%s", buf); std::fflush(stdout);
    }

private:
    BarAccum m_cur_bar{};
    int64_t m_cur_anchor = -1;
    int64_t m_bars_seen = 0;

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

    std::deque<double> m_highs, m_lows;
    bool m_signal_pending=false, m_signal_long=false;
    double m_signal_atr=0;
    int64_t m_cooldown_until=0;

    void _on_bar_close(const BarAccum& bar) {
        if (!m_ema_inited) { m_ema9.init(9); m_ema21.init(21); m_ema_inited=true; }
        m_ema9.push(bar.close); m_ema21.push(bar.close);
        m_atr.push(bar.high, bar.low, bar.close);

        m_highs.push_back(bar.high); m_lows.push_back(bar.low);
        if ((int)m_highs.size() > LOOKBACK+1) { m_highs.pop_front(); m_lows.pop_front(); }

        m_signal_pending=false;
        if (!m_atr.primed || !m_ema9.primed || !m_ema21.primed) return;
        if ((int)m_highs.size() <= LOOKBACK) return;
        if (m_pos.active) return;
        if (_is_weekend(bar.ts_open)) return;
        if (!_is_session_active(bar.ts_open)) return;
        if (m_atr.value < ATR_FLOOR || m_atr.value > ATR_CAP) return;

        double ch_high=-1e18, ch_low=1e18;
        for (int k=(int)m_highs.size()-1-LOOKBACK; k<(int)m_highs.size()-1; ++k) {
            if (k<0) continue;
            if (m_highs[k]>ch_high) ch_high=m_highs[k];
            if (m_lows[k] <ch_low ) ch_low =m_lows[k];
        }
        const bool bull = (bar.close > ch_high);
        const bool bear = (bar.close < ch_low);
        if (!bull && !bear) return;
        const bool intend_long = bull;
        if (intend_long  && m_ema9.value <= m_ema21.value) return;
        if (!intend_long && m_ema9.value >= m_ema21.value) return;

        const double body = std::fabs(bar.close-bar.open);
        const double range = bar.high-bar.low;
        if (range < 0.01) return;
        if (body/range < 0.40) return;
        const double midp = (bar.high+bar.low)*0.5;
        if (intend_long  && bar.close < midp) return;
        if (!intend_long && bar.close > midp) return;

        bool final_long = REVERSE_SIGNAL ? !intend_long : intend_long;
        if (LONG_ONLY && !final_long) return;  // skip short entries in long-only mode

        m_signal_pending = true;
        m_signal_long    = final_long;
        m_signal_atr     = m_atr.value;
    }

    void _manage(double bid, double ask, int64_t now_ms, const CloseCallback* ext_close) {
        if (!m_pos.active) return;
        const double move = m_pos.is_long ? (bid - m_pos.entry) : (m_pos.entry - ask);
        const double adverse = -move;
        if (move > m_pos.mfe_peak) m_pos.mfe_peak = move;
        if (adverse > m_pos.mae) m_pos.mae = adverse;

        if (m_pos.is_long && bid <= m_pos.hard_sl) {
            _close(bid, ask, now_ms, "SL_HIT", ext_close);
            m_cooldown_until = now_ms + COOLDOWN_SEC * 1000LL; return;
        }
        if (!m_pos.is_long && ask >= m_pos.hard_sl) {
            _close(bid, ask, now_ms, "SL_HIT", ext_close);
            m_cooldown_until = now_ms + COOLDOWN_SEC * 1000LL; return;
        }
        if (m_pos.is_long && bid >= m_pos.hard_tp) {
            _close(bid, ask, now_ms, "TP_HIT", ext_close);
            m_cooldown_until = now_ms + COOLDOWN_SEC * 1000LL; return;
        }
        if (!m_pos.is_long && ask <= m_pos.hard_tp) {
            _close(bid, ask, now_ms, "TP_HIT", ext_close);
            m_cooldown_until = now_ms + COOLDOWN_SEC * 1000LL; return;
        }
        const int bars_held = (int)(m_bars_seen - m_pos.entry_bar_seq);
        if (bars_held >= MAX_HOLD_BARS) {
            _close(bid, ask, now_ms, "TIME_STOP", ext_close);
            m_cooldown_until = now_ms + COOLDOWN_SEC * 1000LL; return;
        }
    }

    void _close(double bid, double ask, int64_t now_ms, const char* reason, const CloseCallback* ext_close) {
        const double exit_px = m_pos.is_long ? bid : ask;
        const double move = m_pos.is_long ? (exit_px - m_pos.entry) : (m_pos.entry - exit_px);
        const double pnl_pts_lots = move * m_pos.size;
        const double pnl_usd = pnl_pts_lots * USD_PER_PT_LOT;

        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "[GRR] CLOSE %s entry=%.2f exit=%.2f pnl=$%.2f size=%.3f mfe=%.2f mae=%.2f bars=%d reason=%s\n",
            m_pos.is_long?"LONG":"SHORT", m_pos.entry, exit_px, pnl_usd, m_pos.size,
            m_pos.mfe_peak, m_pos.mae, (int)(m_bars_seen - m_pos.entry_bar_seq), reason);
        std::printf("%s", buf); std::fflush(stdout);

        omega::TradeRecord tr;
        tr.engine="GoldFixedRR"; tr.symbol="XAUUSD";
        tr.side=m_pos.is_long?"LONG":"SHORT"; tr.regime="M5_FIXED_RR";
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
