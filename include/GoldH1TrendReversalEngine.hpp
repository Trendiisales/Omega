#pragma once
//  ADVERSE-PROTECTION: in-flight protection present in code (hard SL/TP bracket SL_ATR_MULT=1.5/TP_ATR_MULT=3.0 + COST_COVER_PTS=0.50 BE ratchet w/ BE_BUFFER_PTS + TRAIL_DIST=2.0 wide trail + TREND_FLIP_EXIT + MAX_HOLD_BARS=6 time-stop), but no faithful backtest on record -- not in AUDITED_CONFIGS/CULL_LEDGER/TOMBSTONES, not wired in engine_init (no g_ instance) -- verdict owed before re-enable (backfill S-2026-06-24n)
// =============================================================================
// GoldH1TrendReversalEngine.hpp -- M5 entry + M1-close reversal-detect exit
// =============================================================================
//
// 2026-05-19 SESSION DESIGN (Claude / Jo):
//   V1 (GoldReversalScalpEngine) used raw tick-direction as the reversal
//   signal. The sweep showed the detector over-fires on bid-ask flutter
//   (~50% of trades exit via REVERSAL_EXIT, mostly false positives), and
//   net PnL was worse than the GSP TICK_LEVEL baseline.
//
//   V2 swaps the signal: instead of tick-direction sign, the detector
//   counts the sign of the last N M1 bar-close moves
//   (close[i] > close[i-1] = +1; equal = 0; lower = -1). This is ~60-3000x
//   smoother than tick-direction (one sample per ~60s instead of per
//   tick), and aligns with the trader concept of "the trend reversed":
//   a string of lower-lows / lower-closes after a long-side entry IS
//   what the trader means.
//
//   Same entry stack as V1 (= GSP best-config). Same cost-cover BE,
//   tight trail. Only the reversal detector changes.
//
//   SWEEP (4 configs):
//     REVERSAL_M1_WINDOW    in {5, 8}      -- last N M1 bars
//     REVERSAL_M1_THRESHOLD in {0.65, 0.75}-- adverse fraction
//
// LOG NAMESPACE:
//   [H1T]. tr.engine = "GoldH1TrendReversal". tr.regime = "H1_TREND_REVERSAL".
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
#include "RegimeState.hpp"       // 2026-06-21: macro-hostile long-block (BearProtect coverage)

namespace omega {

class GoldH1TrendReversalEngine {
public:
    static constexpr int BAR_SECS    = 3600; // H1 entry bars
    static constexpr int M1_BAR_SECS = 60;   // 1min for reversal detector

    int    LOOKBACK     = 24;       // 24 H1 bars = 1 day Donchian
    double SL_ATR_MULT  = 1.5;      // H1 ATR ~5-15 pts -> SL ~7-23 pts
    double TP_ATR_MULT  = 3.0;

    double COST_COVER_PTS = 0.50;   // user-specified cost-cover threshold
    double BE_BUFFER_PTS  = 0.10;
    double TRAIL_DIST     = 2.00;   // wider trail -- H1 winners run further

    // Reversal detector: count of last N M1 bar-close direction signs
    int    REVERSAL_M1_WINDOW       = 15;    // 15 min of M1 direction (~1/4 of H1)
    double REVERSAL_M1_THRESHOLD    = 0.70;
    double REVERSAL_ADVERSE_GATE    = 0.50;

    static constexpr double USD_PER_PT_LOT = 100.0;
    static constexpr double RISK_DOLLARS   = 50.0;
    static constexpr double LOT_MIN        = 0.01;
    static constexpr double LOT_MAX        = 0.05;

    static constexpr double ATR_FLOOR     = 5.00;  // H1 ATR floor
    static constexpr double ATR_CAP       = 50.0;
    static constexpr double SPREAD_CAP    = 0.80;
    static constexpr int    MAX_HOLD_BARS = 6;     // 6 H1 = 6 hours
    static constexpr int    COOLDOWN_SEC  = 1800;  // 30 min between trades

    bool shadow_mode = true;
    bool enabled     = true;

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;
    CloseCallback on_close_cb;

    struct LivePos {
        bool    active          = false;
        bool    is_long         = false;
        bool    be_armed        = false;
        double  entry           = 0.0;
        double  hard_sl         = 0.0;
        double  hard_tp         = 0.0;
        double  trail_sl        = 0.0;
        double  mfe_peak        = 0.0;
        double  mfe_price       = 0.0;
        double  mae             = 0.0;
        double  atr_at_entry    = 0.0;
        double  spread_at_entry = 0.0;
        double  size            = 0.0;
        int64_t entry_ts        = 0;
        int64_t entry_bar_seq   = 0;
    } m_pos;

    bool has_open_position() const noexcept { return m_pos.active; }

    struct BarAccum {
        double open = 0.0, high = 0.0, low = 0.0, close = 0.0;
        int64_t ts_open = 0;
        int n = 0;
    };

    void on_tick(double bid, double ask, int64_t now_ms,
                 bool can_enter,
                 double /*l2_imbalance*/, double /*book_slope*/,
                 bool /*vacuum_ask*/, bool /*vacuum_bid*/,
                 bool /*wall_above*/, bool /*wall_below*/,
                 bool /*l2_real*/,
                 const CloseCallback* ext_close = nullptr)
    {
        if (!enabled) return;

        const double mid    = (bid + ask) * 0.5;
        const double spread = ask - bid;

        _accumulate_m1(mid, now_ms);

        const int64_t m5_ms = BAR_SECS * 1000LL;
        const int64_t m5_anchor = (now_ms / m5_ms) * m5_ms;

        if (m_cur_anchor < 0) {
            m_cur_bar = BarAccum{mid, mid, mid, mid, m5_anchor, 1};
            m_cur_anchor = m5_anchor;
        } else if (m5_anchor != m_cur_anchor) {
            _on_m5_close(m_cur_bar);
            m_cur_bar = BarAccum{mid, mid, mid, mid, m5_anchor, 1};
            m_cur_anchor = m5_anchor;
            ++m_bars_seen;
        } else {
            if (mid > m_cur_bar.high) m_cur_bar.high = mid;
            if (mid < m_cur_bar.low)  m_cur_bar.low  = mid;
            m_cur_bar.close = mid;
            ++m_cur_bar.n;
        }

        if (m_pos.active) {
            _manage_position(bid, ask, now_ms, ext_close);
            return;
        }

        if (!m_signal_pending) return;
        m_signal_pending = false;

        if (!can_enter) return;
        if (now_ms < m_cooldown_until) return;
        if (spread > SPREAD_CAP) return;
        // 2026-06-21: macro-hostile long-block (long side only; bidirectional engine). Fail-safe false.
        if (m_signal_long && omega::gold_regime().long_blocked()) return;

        const double tp_dist = m_signal_atr * TP_ATR_MULT;
        if (!::ExecutionCostGuard::is_viable("XAUUSD", spread, tp_dist, LOT_MIN, 1.5)) return;

        const double sl_dist  = m_signal_atr * SL_ATR_MULT;
        const double entry_px = m_signal_long ? ask : bid;
        const double sl_px    = m_signal_long ? (entry_px - sl_dist) : (entry_px + sl_dist);
        const double tp_px    = m_signal_long ? (entry_px + tp_dist) : (entry_px - tp_dist);

        double size = RISK_DOLLARS / (sl_dist * USD_PER_PT_LOT);
        size = std::floor(size / 0.01) * 0.01;
        size = std::max(LOT_MIN, std::min(LOT_MAX, size));

        m_pos = LivePos{};
        m_pos.active          = true;
        m_pos.is_long         = m_signal_long;
        m_pos.entry           = entry_px;
        m_pos.hard_sl         = sl_px;
        m_pos.hard_tp         = tp_px;
        m_pos.trail_sl        = sl_px;
        m_pos.mfe_price       = entry_px;
        m_pos.atr_at_entry    = m_signal_atr;
        m_pos.spread_at_entry = spread;
        m_pos.size            = size;
        m_pos.entry_ts        = now_ms;
        m_pos.entry_bar_seq   = m_bars_seen;

        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "[H1T] OPEN %s entry=%.2f sl=%.2f tp=%.2f size=%.3f atr=%.2f spread=%.2f shadow=%s\n",
            m_signal_long ? "LONG" : "SHORT",
            entry_px, sl_px, tp_px, size, m_signal_atr, spread,
            shadow_mode ? "true" : "false");
        std::printf("%s", buf);
        std::fflush(stdout);
    }

private:
    // ---- M5 entry bar ----
    BarAccum m_cur_bar{};
    int64_t  m_cur_anchor = -1;
    int64_t  m_bars_seen  = 0;

    // ---- M1 reversal-detector bar accumulator ----
    BarAccum m_m1_bar{};
    int64_t  m_m1_anchor = -1;

    // Reversal detector: window of M1 close-direction signs (+1/0/-1)
    std::deque<int8_t> m_m1_dirs;
    int                m_m1_balance = 0;
    double             m_prev_m1_close = 0.0;

    void _accumulate_m1(double mid, int64_t now_ms) {
        const int64_t bar_ms = M1_BAR_SECS * 1000LL;
        const int64_t a      = (now_ms / bar_ms) * bar_ms;
        if (m_m1_anchor < 0) {
            m_m1_bar = BarAccum{mid, mid, mid, mid, a, 1};
            m_m1_anchor = a;
        } else if (a != m_m1_anchor) {
            _on_m1_close(m_m1_bar);
            m_m1_bar = BarAccum{mid, mid, mid, mid, a, 1};
            m_m1_anchor = a;
        } else {
            if (mid > m_m1_bar.high) m_m1_bar.high = mid;
            if (mid < m_m1_bar.low)  m_m1_bar.low  = mid;
            m_m1_bar.close = mid;
            ++m_m1_bar.n;
        }
    }

    void _on_m1_close(const BarAccum& b) {
        int8_t dir = 0;
        if (m_prev_m1_close > 0.0) {
            if      (b.close > m_prev_m1_close) dir = +1;
            else if (b.close < m_prev_m1_close) dir = -1;
        }
        m_prev_m1_close = b.close;
        m_m1_dirs.push_back(dir);
        m_m1_balance += dir;
        while ((int)m_m1_dirs.size() > REVERSAL_M1_WINDOW) {
            m_m1_balance -= m_m1_dirs.front();
            m_m1_dirs.pop_front();
        }
    }

    double _adverse_fraction(bool is_long) const {
        if ((int)m_m1_dirs.size() < REVERSAL_M1_WINDOW) return 0.0;
        const double W = (double)REVERSAL_M1_WINDOW;
        const double signed_bal = is_long ? (double)m_m1_balance : -(double)m_m1_balance;
        return (W - signed_bal) / (2.0 * W);
    }

    // ---- M5 indicators ----
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
    bool     m_ema_inited = false;

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

    std::deque<double> m_highs, m_lows;

    bool   m_signal_pending = false;
    bool   m_signal_long    = false;
    double m_signal_atr     = 0.0;

    int64_t m_cooldown_until = 0;

    void _on_m5_close(const BarAccum& bar) {
        if (!m_ema_inited) {
            m_ema9.init(9);
            m_ema21.init(21);
            m_ema_inited = true;
        }
        m_ema9.push(bar.close);
        m_ema21.push(bar.close);
        m_atr.push(bar.high, bar.low, bar.close);

        m_highs.push_back(bar.high);
        m_lows.push_back(bar.low);
        if ((int)m_highs.size() > LOOKBACK + 1) { m_highs.pop_front(); m_lows.pop_front(); }

        m_signal_pending = false;

        if (!m_atr.primed) return;
        if (!m_ema9.primed || !m_ema21.primed) return;
        if ((int)m_highs.size() <= LOOKBACK) return;
        if (m_pos.active) return;

        if (_is_weekend(bar.ts_open)) return;
        if (!_is_session_active(bar.ts_open)) return;

        if (m_atr.value < ATR_FLOOR || m_atr.value > ATR_CAP) return;

        double ch_high = -1e18, ch_low = 1e18;
        if ((int)m_highs.size() <= LOOKBACK) return;
        for (int k = (int)m_highs.size() - 1 - LOOKBACK; k < (int)m_highs.size() - 1; ++k) {
            if (k < 0) continue;
            if (m_highs[k] > ch_high) ch_high = m_highs[k];
            if (m_lows[k]  < ch_low)  ch_low  = m_lows[k];
        }

        const bool bull_break = (bar.close > ch_high);
        const bool bear_break = (bar.close < ch_low);
        if (!bull_break && !bear_break) return;

        const bool intend_long = bull_break;
        if (intend_long  && m_ema9.value <= m_ema21.value) return;
        if (!intend_long && m_ema9.value >= m_ema21.value) return;

        const double body  = std::fabs(bar.close - bar.open);
        const double range = bar.high - bar.low;
        if (range < 0.01) return;
        if (body / range < 0.40) return;
        const double mid_price = (bar.high + bar.low) * 0.5;
        if (intend_long  && bar.close < mid_price) return;
        if (!intend_long && bar.close > mid_price) return;

        m_signal_pending = true;
        m_signal_long    = intend_long;
        m_signal_atr     = m_atr.value;
    }

    void _manage_position(double bid, double ask, int64_t now_ms,
                          const CloseCallback* ext_close)
    {
        if (!m_pos.active) return;

        const double move = m_pos.is_long
            ? (bid - m_pos.entry)
            : (m_pos.entry - ask);
        const double adverse = -move;

        if (move > m_pos.mfe_peak) {
            m_pos.mfe_peak  = move;
            m_pos.mfe_price = m_pos.is_long ? bid : ask;
        }
        if (adverse > m_pos.mae) m_pos.mae = adverse;

        const double sl_dist = m_pos.atr_at_entry * SL_ATR_MULT;

        if (!m_pos.be_armed && m_pos.mfe_peak >= COST_COVER_PTS) {
            m_pos.be_armed = true;
            const double be = m_pos.is_long
                ? (m_pos.entry + BE_BUFFER_PTS)
                : (m_pos.entry - BE_BUFFER_PTS);
            if (m_pos.is_long) m_pos.trail_sl = std::max(m_pos.trail_sl, be);
            else               m_pos.trail_sl = std::min(m_pos.trail_sl, be);
        }

        if (m_pos.be_armed) {
            const double t = m_pos.is_long
                ? (m_pos.mfe_price - TRAIL_DIST)
                : (m_pos.mfe_price + TRAIL_DIST);
            if (m_pos.is_long) m_pos.trail_sl = std::max(m_pos.trail_sl, t);
            else               m_pos.trail_sl = std::min(m_pos.trail_sl, t);
        }

        // ---- TREND-REVERSAL EXIT (per user goal 2026-05-19 part-C) ----
        // Exit when the entry-direction trend signal (EMA9 vs EMA21 on
        // the entry timeframe) flips against the position. The EMAs
        // update only on bar-close so the check is effectively per-bar
        // resolution, but evaluating each tick costs ~1 compare and the
        // close fires on the first qualifying tick.
        //
        // Gate: only fires after BE armed OR adverse > GATE * SL_dist.
        // Prevents instant exits on entry-tick chop.
        if (m_ema9.primed && m_ema21.primed) {
            const bool gate_open =
                m_pos.be_armed || (adverse >= sl_dist * REVERSAL_ADVERSE_GATE);
            if (gate_open) {
                const bool trend_against =
                    (m_pos.is_long  && m_ema9.value < m_ema21.value) ||
                    (!m_pos.is_long && m_ema9.value > m_ema21.value);
                if (trend_against) {
                    _close_position(bid, ask, now_ms, "TREND_FLIP_EXIT", ext_close);
                    m_cooldown_until = now_ms + COOLDOWN_SEC * 1000LL;
                    return;
                }
            }
        }

        const double eff_sl = m_pos.is_long
            ? std::max(m_pos.hard_sl, m_pos.trail_sl)
            : std::min(m_pos.hard_sl, m_pos.trail_sl);

        if (m_pos.is_long && bid <= eff_sl) {
            const char* reason = (m_pos.trail_sl > m_pos.hard_sl) ? "TRAIL_HIT" : "SL_HIT";
            _close_position(bid, ask, now_ms, reason, ext_close);
            m_cooldown_until = now_ms + COOLDOWN_SEC * 1000LL;
            return;
        }
        if (!m_pos.is_long && ask >= eff_sl) {
            const char* reason = (m_pos.trail_sl < m_pos.hard_sl) ? "TRAIL_HIT" : "SL_HIT";
            _close_position(bid, ask, now_ms, reason, ext_close);
            m_cooldown_until = now_ms + COOLDOWN_SEC * 1000LL;
            return;
        }

        if (m_pos.is_long && bid >= m_pos.hard_tp) {
            _close_position(bid, ask, now_ms, "TP_HIT", ext_close);
            m_cooldown_until = now_ms + COOLDOWN_SEC * 1000LL;
            return;
        }
        if (!m_pos.is_long && ask <= m_pos.hard_tp) {
            _close_position(bid, ask, now_ms, "TP_HIT", ext_close);
            m_cooldown_until = now_ms + COOLDOWN_SEC * 1000LL;
            return;
        }

        const int bars_held = (int)(m_bars_seen - m_pos.entry_bar_seq);
        if (bars_held >= MAX_HOLD_BARS) {
            _close_position(bid, ask, now_ms, "TIME_STOP", ext_close);
            m_cooldown_until = now_ms + COOLDOWN_SEC * 1000LL;
            return;
        }
    }

    void _close_position(double bid, double ask, int64_t now_ms,
                         const char* reason,
                         const CloseCallback* ext_close)
    {
        const double exit_px = m_pos.is_long ? bid : ask;
        const double move = m_pos.is_long
            ? (exit_px - m_pos.entry)
            : (m_pos.entry - exit_px);
        const double pnl_pts_lots = move * m_pos.size;
        const double pnl_usd      = pnl_pts_lots * USD_PER_PT_LOT;

        char buf[384];
        std::snprintf(buf, sizeof(buf),
            "[H1T] CLOSE %s entry=%.2f exit=%.2f pnl=$%.2f size=%.3f "
            "mfe=%.2f mae=%.2f be=%d bars=%d reason=%s shadow=%s\n",
            m_pos.is_long ? "LONG" : "SHORT",
            m_pos.entry, exit_px, pnl_usd, m_pos.size,
            m_pos.mfe_peak, m_pos.mae,
            (int)m_pos.be_armed,
            (int)(m_bars_seen - m_pos.entry_bar_seq),
            reason, shadow_mode ? "true" : "false");
        std::printf("%s", buf);
        std::fflush(stdout);

        omega::TradeRecord tr;
        tr.engine     = "GoldH1TrendReversal";
        tr.symbol     = "XAUUSD";
        tr.side       = m_pos.is_long ? "LONG" : "SHORT";
        tr.regime     = "H1_TREND_REVERSAL";
        tr.entryPrice = m_pos.entry;
        tr.exitPrice  = exit_px;
        tr.size       = m_pos.size;
        tr.pnl        = pnl_pts_lots;
        tr.entryTs    = m_pos.entry_ts / 1000LL;
        tr.exitTs     = now_ms / 1000LL;
        tr.exitReason = reason;
        tr.mfe        = m_pos.mfe_peak * m_pos.size;
        tr.mae        = m_pos.mae      * m_pos.size;
        tr.spreadAtEntry = m_pos.spread_at_entry;
        tr.shadow     = shadow_mode;

        if (ext_close && *ext_close) (*ext_close)(tr);
        else if (on_close_cb)        on_close_cb(tr);

        m_pos = LivePos{};
    }

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
