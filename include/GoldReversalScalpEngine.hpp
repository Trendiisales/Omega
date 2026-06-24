#pragma once
//  ADVERSE-PROTECTION: in-flight stack present in code (hard SL=2.0xATR + cost-cover BE arm @0.50pt -> trail 0.30pt + tick-imbalance REVERSAL_EXIT + 12-bar TIME_STOP), but no faithful backtest on record -- verdict owed before re-enable; engine is shadow_mode=true and NOT wired into engine_init.hpp (harness-only by design), no AUDITED_CONFIGS/CULL/TOMBSTONE entry. (backfill S-2026-06-24n)
// =============================================================================
// GoldReversalScalpEngine.hpp -- M5 Gold Scalp with cost-cover BE + tight trail
//                                + tick-imbalance reversal-detect exit
// =============================================================================
//
// 2026-05-19 SESSION DESIGN (Claude / Jo):
//   Hypothesis: S101's null result on trail-philosophy variants (TICK /
//   BAR_CLOSE / GIVE_BACK) showed avgWin ~$5.50 vs avgLoss ~$21.78 on a
//   71% WR shape -- needs 80% WR to break even. The avgLoss floor is the
//   1.5xATR hard SL. If we can detect a tick-level price reversal BEFORE
//   the hard SL fires, avgLoss tightens; the same detector locks profit
//   when it fires after a winner peaks. Asymmetric -> shrinks loss tail.
//
//   Entry stack: same as GSP best-config (M5 Donchian-break + EMA9/EMA21
//   trend gate + momentum-bar filter + session 07-21 UTC + ATR floor/cap).
//   This is proven 71% WR; the entry isn't the issue.
//
//   Exit stack (this is the experiment):
//     1. Hard SL at SL_ATR_MULT * ATR  (backstop; wider than GSP so
//        the reversal detector catches losers first)
//     2. Hard TP at TP_ATR_MULT * ATR  (runners)
//     3. BE arm: once MFE >= COST_COVER_PTS, ratchet trail SL to
//        entry + BE_BUFFER_PTS  (cost-covered breakeven floor)
//     4. Tight trail: after BE armed, trail SL = mfe_price - TRAIL_DIST
//        (ratcheting, only moves favorable)
//     5. Tick-imbalance reversal detector:
//        Rolling deque of last REVERSAL_WINDOW tick directions
//          (+1 / 0 / -1 on each new tick mid vs prev mid).
//        Adverse fraction = (W - signed_balance * dir_sign) / (2W)
//          where dir_sign = +1 long, -1 short.
//        Exit immediately if adverse_frac >= REVERSAL_THRESHOLD AND
//          (be_armed OR adverse_pts >= SL_dist * REVERSAL_ADVERSE_GATE).
//        Gate prevents the detector from firing on early-entry chop
//        before the position has had time to develop.
//
// SAFETY:
//   shadow_mode = true by default. NOT wired into engine_init.hpp at
//   commit time -- harness-only until results show edge.
//
// LOG NAMESPACE:
//   All log lines use prefix [GRS]. tr.engine = "GoldReversalScalp".
//   tr.regime = "M5_REVERSAL_SCALP".
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

class GoldReversalScalpEngine {
public:
    // ---- Timing ---------------------------------------------------------------
    static constexpr int BAR_SECS = 300;  // 5 minutes

    // ---- Entry params (same as GSP best-config) -------------------------------
    int    LOOKBACK     = 8;
    double SL_ATR_MULT  = 2.0;   // wider than GSP (1.5) -- reversal-detect first
    double TP_ATR_MULT  = 3.0;

    // ---- Cost-cover BE + trail ------------------------------------------------
    double COST_COVER_PTS = 0.50;  // ~0.30 spread + 0.20 slippage budget
    double BE_BUFFER_PTS  = 0.10;  // SL = entry + 0.10 after BE arm (cost-cover)
    double TRAIL_DIST     = 0.30;  // pts retained behind mfe_price after BE

    // ---- Reversal detector ----------------------------------------------------
    int    REVERSAL_WINDOW       = 50;    // last N ticks counted
    double REVERSAL_THRESHOLD    = 0.62;  // adverse fraction to fire
    double REVERSAL_ADVERSE_GATE = 0.50;  // fraction of SL_dist before detector
                                          // can fire on a non-BE-armed position

    // ---- Sizing ---------------------------------------------------------------
    static constexpr double USD_PER_PT_LOT = 100.0;
    static constexpr double RISK_DOLLARS   = 50.0;
    static constexpr double LOT_MIN        = 0.01;
    static constexpr double LOT_MAX        = 0.05;

    // ---- Cost gate model ------------------------------------------------------
    static constexpr double COST_RT_PTS = 0.50;
    static constexpr double HALF_SPREAD = 0.25;

    // ---- Session / filter constants -------------------------------------------
    static constexpr double ATR_FLOOR     = 1.50;
    static constexpr double ATR_CAP       = 15.0;
    static constexpr double SPREAD_CAP    = 0.80;
    static constexpr int    MAX_HOLD_BARS = 12;   // 60 minutes on M5
    static constexpr int    COOLDOWN_SEC  = 60;

    // ---- Public state ---------------------------------------------------------
    bool shadow_mode = true;
    bool enabled     = true;

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;
    CloseCallback on_close_cb;

    // ---- Position tracking ----------------------------------------------------
    struct LivePos {
        bool    active          = false;
        bool    is_long         = false;
        bool    be_armed        = false;
        double  entry           = 0.0;
        double  hard_sl         = 0.0;
        double  hard_tp         = 0.0;
        double  trail_sl        = 0.0;
        double  mfe_peak        = 0.0;   // points
        double  mfe_price       = 0.0;
        double  mae             = 0.0;
        double  atr_at_entry    = 0.0;
        double  spread_at_entry = 0.0;
        double  size            = 0.0;
        int64_t entry_ts        = 0;
        int64_t entry_bar_seq   = 0;
    } m_pos;

    bool has_open_position() const noexcept { return m_pos.active; }

    // ---- M5 bar ---------------------------------------------------------------
    struct M5Bar {
        double open = 0.0, high = 0.0, low = 0.0, close = 0.0;
        int64_t ts_open = 0;
        int n = 0;
    };

    // =========================================================================
    // Public: feed every XAUUSD tick. l2_* args kept in the signature for
    // dispatch-compatibility but ignored (this engine is tape-only).
    // =========================================================================
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

        // ----- Tick-reversal detector: update first so manage sees fresh state -----
        _push_tick_direction(mid);

        // ----- Bar accumulation -----
        const int64_t bar_ms = BAR_SECS * 1000LL;
        const int64_t anchor = (now_ms / bar_ms) * bar_ms;

        if (m_cur_anchor < 0) {
            m_cur_bar = M5Bar{mid, mid, mid, mid, anchor, 1};
            m_cur_anchor = anchor;
        } else if (anchor != m_cur_anchor) {
            _on_bar_close(m_cur_bar);
            m_cur_bar = M5Bar{mid, mid, mid, mid, anchor, 1};
            m_cur_anchor = anchor;
            ++m_bars_seen;
        } else {
            if (mid > m_cur_bar.high) m_cur_bar.high = mid;
            if (mid < m_cur_bar.low)  m_cur_bar.low  = mid;
            m_cur_bar.close = mid;
            ++m_cur_bar.n;
        }

        // ----- Manage open position (per-tick) -----
        if (m_pos.active) {
            _manage_position(bid, ask, now_ms, ext_close);
            return;
        }

        // ----- Entry check (only on bar-close ticks) -----
        if (!m_signal_pending) return;
        m_signal_pending = false;

        if (!can_enter) return;
        if (now_ms < m_cooldown_until) return;
        if (spread > SPREAD_CAP) return;

        // Cost gate
        double tp_dist = m_signal_atr * TP_ATR_MULT;
        if (!::ExecutionCostGuard::is_viable("XAUUSD", spread, tp_dist, LOT_MIN, 1.5)) return;

        // ---- Open position ----
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
        m_pos.be_armed        = false;
        m_pos.entry           = entry_px;
        m_pos.hard_sl         = sl_px;
        m_pos.hard_tp         = tp_px;
        m_pos.trail_sl        = sl_px;
        m_pos.mfe_peak        = 0.0;
        m_pos.mfe_price       = entry_px;
        m_pos.mae             = 0.0;
        m_pos.atr_at_entry    = m_signal_atr;
        m_pos.spread_at_entry = spread;
        m_pos.size            = size;
        m_pos.entry_ts        = now_ms;
        m_pos.entry_bar_seq   = m_bars_seen;

        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "[GRS] OPEN %s entry=%.2f sl=%.2f tp=%.2f size=%.3f atr=%.2f spread=%.2f shadow=%s\n",
            m_signal_long ? "LONG" : "SHORT",
            entry_px, sl_px, tp_px, size, m_signal_atr, spread,
            shadow_mode ? "true" : "false");
        std::printf("%s", buf);
        std::fflush(stdout);
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

    // ---- Donchian channel ----
    std::deque<double> m_highs, m_lows;

    // ---- Signal state ----
    bool   m_signal_pending = false;
    bool   m_signal_long    = false;
    double m_signal_atr     = 0.0;

    // ---- Cooldown ----
    int64_t m_cooldown_until = 0;

    // ---- Tick-imbalance reversal detector state ----
    // Sliding window of last REVERSAL_WINDOW tick direction signs.
    // Single signed running sum so the test is O(1) per tick.
    //   m_tick_balance \in [-W, +W]
    //   adverse_frac (long)  = (W - balance) / (2 * W)   so balance=-W -> 1.0
    //   adverse_frac (short) = (W + balance) / (2 * W)   so balance=+W -> 1.0
    std::deque<int8_t> m_tick_dirs;
    int    m_tick_balance = 0;
    double m_prev_mid     = 0.0;

    void _push_tick_direction(double mid) {
        int8_t dir = 0;
        if (m_prev_mid > 0.0) {
            if      (mid > m_prev_mid) dir = +1;
            else if (mid < m_prev_mid) dir = -1;
        }
        m_prev_mid = mid;
        m_tick_dirs.push_back(dir);
        m_tick_balance += dir;
        while ((int)m_tick_dirs.size() > REVERSAL_WINDOW) {
            m_tick_balance -= m_tick_dirs.front();
            m_tick_dirs.pop_front();
        }
    }

    // Returns adverse_frac in [0, 1]. Returns 0 (no signal) if window not full.
    double _adverse_fraction(bool is_long) const {
        if ((int)m_tick_dirs.size() < REVERSAL_WINDOW) return 0.0;
        const double W = (double)REVERSAL_WINDOW;
        const double bal = (double)m_tick_balance;
        // For long: adverse = downticks dominate -> balance negative
        // For short: adverse = upticks dominate   -> balance positive
        const double signed_bal = is_long ? bal : -bal;
        // adverse_frac = (W - signed_bal) / (2W)
        return (W - signed_bal) / (2.0 * W);
    }

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

        // Donchian over prior LOOKBACK bars (exclude current)
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

    // =========================================================================
    // Manage position: SL/TP + cost-cover BE + tight trail + reversal-detect
    // =========================================================================
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

        // ---- BE arm: once cost is covered, lift SL to entry + buffer ----
        if (!m_pos.be_armed && m_pos.mfe_peak >= COST_COVER_PTS) {
            m_pos.be_armed = true;
            const double be = m_pos.is_long
                ? (m_pos.entry + BE_BUFFER_PTS)
                : (m_pos.entry - BE_BUFFER_PTS);
            if (m_pos.is_long) m_pos.trail_sl = std::max(m_pos.trail_sl, be);
            else               m_pos.trail_sl = std::min(m_pos.trail_sl, be);
        }

        // ---- Tight trail (post-BE only): retain TRAIL_DIST behind mfe_price ----
        if (m_pos.be_armed) {
            const double t = m_pos.is_long
                ? (m_pos.mfe_price - TRAIL_DIST)
                : (m_pos.mfe_price + TRAIL_DIST);
            if (m_pos.is_long) m_pos.trail_sl = std::max(m_pos.trail_sl, t);
            else               m_pos.trail_sl = std::min(m_pos.trail_sl, t);
        }

        // ---- Reversal-detect exit ----
        // Gated: fires only after BE armed OR after adverse > REVERSAL_ADVERSE_GATE * SL_dist.
        // Prevents firing on entry-tick chop before position has had room to develop.
        {
            const bool gate_open =
                m_pos.be_armed || (adverse >= sl_dist * REVERSAL_ADVERSE_GATE);
            if (gate_open) {
                const double af = _adverse_fraction(m_pos.is_long);
                if (af >= REVERSAL_THRESHOLD) {
                    _close_position(bid, ask, now_ms, "REVERSAL_EXIT", ext_close);
                    m_cooldown_until = now_ms + COOLDOWN_SEC * 1000LL;
                    return;
                }
            }
        }

        // ---- Hard exits ----
        // Effective SL = the further-from-entry of {hard_sl, trail_sl} once ratcheted.
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

        // Time stop
        const int bars_held = (int)(m_bars_seen - m_pos.entry_bar_seq);
        if (bars_held >= MAX_HOLD_BARS) {
            _close_position(bid, ask, now_ms, "TIME_STOP", ext_close);
            m_cooldown_until = now_ms + COOLDOWN_SEC * 1000LL;
            return;
        }
    }

    // =========================================================================
    // Close: fire TradeRecord (pts*lots convention, S99h)
    // =========================================================================
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
            "[GRS] CLOSE %s entry=%.2f exit=%.2f pnl=$%.2f size=%.3f "
            "mfe=%.2f mae=%.2f be=%d bars=%d reason=%s shadow=%s\n",
            m_pos.is_long ? "LONG" : "SHORT",
            m_pos.entry, exit_px, pnl_usd, m_pos.size,
            m_pos.mfe_peak, m_pos.mae,
            (int)m_pos.be_armed,
            (int)(m_bars_seen - m_pos.entry_bar_seq),
            reason,
            shadow_mode ? "true" : "false");
        std::printf("%s", buf);
        std::fflush(stdout);

        omega::TradeRecord tr;
        tr.engine     = "GoldReversalScalp";
        tr.symbol     = "XAUUSD";
        tr.side       = m_pos.is_long ? "LONG" : "SHORT";
        tr.regime     = "M5_REVERSAL_SCALP";
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

    // ---- Time helpers (same shape as GSP) ----
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
