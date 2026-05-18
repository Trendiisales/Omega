#pragma once
// =============================================================================
// GoldSessionBreakoutEngine.hpp -- M5 Donchian breakout, NY-focused, H4-gated
// =============================================================================
//
// 2026-05-18 SESSION DESIGN (Claude / Jo) -- diagnostic-grounded variant
//
// LINEAGE:
//   This engine is a deliberate CONSTRAINED VARIATION of GoldScalpPyramid
//   (GSP), whose 162-config sweep is the only positive-edge gold M5 result
//   in the repo (best PF 1.45, +$15083 over 26 months, 71% WR, max DD only
//   $454, profitable in 25 of 26 months -- see backtest/gold_scalp_pyramid_
//   results.txt for the full evidence).
//
//   The GSP "best config" trade log reveals three additive insights that
//   this engine bakes into its filter set:
//
//   1. NY 12-17 UTC carries the system. NY first 5 hours produced ~63% of
//      gross profit on ~55% of trade count. WR jumps from 65-69% in London
//      to 72-77% in NY. London is profitable but quieter; this engine
//      cuts London entirely to lift expectancy.
//
//   2. H4 trend alignment removes the dominant loss cluster. GSP's top-10
//      losses are mostly counter-trend M5 breakouts mid-trend (e.g. SHORT
//      at 5128 in March 2026 when H4 trend was UP). g_trend_pb_gold uses
//      this same gate and has paper-validated edge in the codebase.
//
//   3. The signal is bimodal -- winners' avg MFE is 3.05pt, losers' avg
//      MFE is 0.26pt. The trade either works fast or doesn't work at all.
//      An ATR-of-ATR squeeze filter (skip when current M5 ATR is too far
//      above the long-window ATR average) culls news-spike bars that
//      cluster in the loser MFE=0 bucket.
//
//   Everything else is held identical to GSP's proven best-config core:
//      Donchian lookback 8, SL=1.5xATR, TP=3.0xATR (decorative), trail
//      0.12xATR, 4-phase trail, pyramid 5 layers max. Sizing, cost gate,
//      cooldown, session weekend logic all identical.
//
// HYPOTHESIS (testable):
//   The three new filters trade trade-count for win-rate. Expected
//   1500-2500 trades over 2yr (vs GSP 5436) but PF 1.5-1.8 (vs 1.45),
//   max DD $300-450 (vs $454). If the hypothesis fails, the sweep will
//   show fewer trades AND degraded PF -- in which case revert to GSP
//   shape exactly.
//
// CONVENTIONS (S99h fix 2026-05-18 part C):
//   tr.pnl/mfe/mae are reported in points*lots units. trade_lifecycle.hpp
//   :218-224 multiplies by tick_value_multiplier(symbol)=100 for XAUUSD
//   downstream. spreadAtEntry MUST be populated for apply_realistic_costs
//   to compute slippage. See GoldScalpPyramidEngine.hpp for the full
//   rationale and S99j for the spreadAtEntry follow-up.
//
// SAFETY:
//   enabled = false by default. Do NOT promote to live until standalone
//   harness (backtest/gold_session_breakout_bt.cpp) confirms the
//   hypothesis on fresh tape. Shadow_mode then true until 2-week paper
//   validation confirms.
//
// LOG NAMESPACE:
//   [GSB] prefix on all log lines.
//   tr.engine = "GoldSessionBreakout".
//   tr.regime = "M5_BREAKOUT_NY".
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

class GoldSessionBreakoutEngine {
public:
    // ---- Timing ---------------------------------------------------------------
    static constexpr int BAR_SECS = 300;  // 5 minutes

    // ---- Donchian lookback (GSP best: 8) --------------------------------------
    int LOOKBACK = 8;

    // ---- Risk parameters (GSP best: SL=1.5, TP=3.0, Trail=0.12) ---------------
    double SL_ATR_MULT   = 1.5;
    double TP_ATR_MULT   = 3.0;   // decorative -- trail does the real work
    double TRAIL_TIGHT   = 0.12;

    // ---- Pyramid (GSP best: ON, 5 layers max) ---------------------------------
    bool   PYRAMID_ON    = true;
    static constexpr int MAX_LAYERS = 5;

    // ---- S63 in-flight protection (mirror GSP) --------------------------------
    double LOSS_CUT_PCT  = 0.05;
    double BE_ARM_PCT    = 0.03;
    double BE_BUFFER_PCT = 0.012;

    // ---- NEW FILTER #1: Session window (default NY 12-17 UTC, GSP's peak) ----
    // Set to 7/21 to match GSP exactly (no filter beyond London+NY).
    // Set to 12/17 for NY-only (this engine's default + hypothesis).
    int SESSION_START_HOUR_UTC = 12;
    int SESSION_END_HOUR_UTC   = 17;

    // ---- NEW FILTER #2: H4 trend gate ----------------------------------------
    // When enabled, LONG entries require H4 trend = +1, SHORT entries require
    // H4 trend = -1. When trend = 0 (flat) all entries blocked.
    // H4 trend value is passed in on_tick() from caller (g_bars_gold.h4.ind
    // .trend_state in production; harness-computed in backtest).
    bool H4_GATE_ENABLED = true;

    // ---- NEW FILTER #3: ATR-of-ATR squeeze filter ----------------------------
    // Skip entries when current ATR(M5,14) > ATR_SPIKE_MULT * ATR_AVG.
    // ATR_AVG is the 50-bar running mean of ATR14. This culls news-spike bars
    // (the cluster where GSP loser MFE=0.26 bucket lives).
    // Set ATR_SPIKE_MULT very high (e.g. 99.0) to effectively disable.
    double ATR_SPIKE_MULT = 2.0;
    int    ATR_AVG_LOOKBACK = 50;

    // ---- L2 (optional soft gate, default off for backtest validity) ----------
    bool   L2_GATE_ENABLED   = false;
    double L2_IMBAL_LONG_MIN = 0.40;  // soft -- not strongly ask-heavy
    double L2_IMBAL_SHORT_MAX= 0.60;  // soft -- not strongly bid-heavy

    // ---- Sizing (mirror GSP) -------------------------------------------------
    static constexpr double USD_PER_PT_LOT = 100.0;  // for sizing math only
    static constexpr double RISK_DOLLARS   = 50.0;
    static constexpr double LOT_MIN        = 0.01;
    static constexpr double LOT_MAX        = 0.05;

    // ---- Cost / session constants (mirror GSP) -------------------------------
    static constexpr double COST_RT_PTS    = 0.50;
    static constexpr double ATR_FLOOR      = 1.50;
    static constexpr double ATR_CAP        = 15.0;
    static constexpr double SPREAD_CAP     = 0.80;
    static constexpr int    MAX_HOLD_BARS  = 12;   // 60 minutes on M5
    static constexpr int    COOLDOWN_SEC   = 60;

    // ---- Public state --------------------------------------------------------
    bool shadow_mode = true;
    bool enabled     = false;   // OFF by default until sweep validates

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;
    CloseCallback on_close_cb;

    // ---- Position tracking (single position, optional pyramid) --------------
    struct Layer {
        bool   active = false;
        double entry  = 0.0;
        double size   = 0.0;
    };

    struct LivePos {
        bool    active        = false;
        bool    is_long       = false;
        double  base_entry    = 0.0;
        double  hard_sl       = 0.0;
        double  hard_tp       = 0.0;
        double  trail_sl      = 0.0;
        double  mfe_peak      = 0.0;
        double  mfe_price     = 0.0;
        double  mae           = 0.0;
        double  atr_at_entry  = 0.0;
        double  spread_at_entry = 0.0;
        int64_t entry_ts      = 0;
        int64_t entry_bar_seq = 0;
        int     h4_trend_at_entry = 0;
        Layer   layers[MAX_LAYERS];
        int     n_layers      = 0;
        int     next_pyramid_idx = 1;

        double total_size() const {
            double s = 0.0;
            for (int i = 0; i < MAX_LAYERS; ++i) if (layers[i].active) s += layers[i].size;
            return s;
        }
        double weighted_entry() const {
            double sv = 0.0, ss = 0.0;
            for (int i = 0; i < MAX_LAYERS; ++i) {
                if (layers[i].active) { sv += layers[i].entry * layers[i].size; ss += layers[i].size; }
            }
            return ss > 0.0 ? sv / ss : 0.0;
        }
    } m_pos;

    bool has_open_position() const noexcept { return m_pos.active; }

    // ---- M5 bar --------------------------------------------------------------
    struct M5Bar {
        double open = 0.0, high = 0.0, low = 0.0, close = 0.0;
        int64_t ts_open = 0;
        int n = 0;
    };

    // ==========================================================================
    // Public interface: feed every XAUUSD tick.
    //
    //   h4_trend_state : -1=H4 downtrend, +1=H4 uptrend, 0=flat/unknown.
    //                    In production: g_bars_gold.h4.ind.trend_state atomic.
    //                    In backtest:   harness-computed from H4 bar history.
    //   l2_imbalance   : optional, ignored unless L2_GATE_ENABLED.
    //   l2_real        : optional, ignored unless L2_GATE_ENABLED.
    // ==========================================================================
    void on_tick(double bid, double ask, int64_t now_ms,
                 bool can_enter,
                 int h4_trend_state,
                 double l2_imbalance = 0.5,
                 bool l2_real = false)
    {
        if (!enabled) return;

        const double mid    = (bid + ask) * 0.5;
        const double spread = ask - bid;

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
            _manage_position(bid, ask, now_ms);
            return;
        }

        // ----- Entry check (only fires when bar-close has set signal_pending) --
        if (!m_signal_pending) return;
        m_signal_pending = false;

        if (!can_enter) return;
        if (now_ms < m_cooldown_until) return;
        if (spread > SPREAD_CAP) return;

        // ---- NEW FILTER #2: H4 trend gate ----
        if (H4_GATE_ENABLED) {
            if (m_signal_long  && h4_trend_state <= 0) return;
            if (!m_signal_long && h4_trend_state >= 0) return;
        }

        // ---- NEW FILTER #3: ATR-of-ATR squeeze ----
        if (ATR_SPIKE_MULT > 0.0 && m_atr_avg > 0.0) {
            if (m_signal_atr > ATR_SPIKE_MULT * m_atr_avg) return;
        }

        // ---- L2 (optional) ----
        if (L2_GATE_ENABLED && l2_real) {
            if (m_signal_long  && l2_imbalance < L2_IMBAL_LONG_MIN) return;
            if (!m_signal_long && l2_imbalance > L2_IMBAL_SHORT_MAX) return;
        }

        // ---- Cost gate ----
        const double tp_dist = m_signal_atr * TP_ATR_MULT;
        if (!::ExecutionCostGuard::is_viable("XAUUSD", spread, tp_dist, LOT_MIN, 1.5)) return;

        // ---- Open position ----
        const double sl_dist  = m_signal_atr * SL_ATR_MULT;
        const double entry_px = m_signal_long ? ask : bid;
        const double sl_px    = m_signal_long ? (entry_px - sl_dist) : (entry_px + sl_dist);
        const double tp_px    = m_signal_long ? (entry_px + tp_dist) : (entry_px - tp_dist);

        // Size from risk budget
        double size = RISK_DOLLARS / (sl_dist * USD_PER_PT_LOT);
        size = std::floor(size / 0.01) * 0.01;
        size = std::max(LOT_MIN, std::min(LOT_MAX, size));

        m_pos                 = LivePos{};
        m_pos.active          = true;
        m_pos.is_long         = m_signal_long;
        m_pos.base_entry      = entry_px;
        m_pos.hard_sl         = sl_px;
        m_pos.hard_tp         = tp_px;
        m_pos.trail_sl        = sl_px;
        m_pos.mfe_peak        = 0.0;
        m_pos.mfe_price       = entry_px;
        m_pos.atr_at_entry    = m_signal_atr;
        m_pos.spread_at_entry = spread;
        m_pos.entry_ts        = now_ms;
        m_pos.entry_bar_seq   = m_bars_seen;
        m_pos.h4_trend_at_entry = h4_trend_state;
        m_pos.layers[0]       = {true, entry_px, size};
        m_pos.n_layers        = 1;
        m_pos.next_pyramid_idx = 1;

        char buf[480];
        std::snprintf(buf, sizeof(buf),
            "[GSB] OPEN %s entry=%.2f sl=%.2f tp=%.2f size=%.3f atr=%.2f atr_avg=%.2f "
            "h4_trend=%+d spread=%.2f shadow=%s\n",
            m_signal_long ? "LONG" : "SHORT",
            entry_px, sl_px, tp_px, size, m_signal_atr, m_atr_avg,
            h4_trend_state, spread,
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
            if (!primed) {
                value += v; ++count;
                if (count >= period) { value /= period; primed = true; }
            } else {
                value = alpha * v + (1.0 - alpha) * value;
            }
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
            if (!have_prev) tr = h - l;
            else {
                const double a = h - l;
                const double b = std::fabs(h - prev_close);
                const double cc = std::fabs(l - prev_close);
                tr = std::max(a, std::max(b, cc));
            }
            have_prev = true; prev_close = c;
            if (!primed) {
                seed.push_back(tr);
                if ((int)seed.size() >= 14) {
                    double s = 0.0; for (auto v : seed) s += v;
                    value = s / 14.0; primed = true;
                }
            } else {
                value = (value * 13.0 + tr) / 14.0;
            }
        }
    } m_atr;

    // ---- ATR-of-ATR (rolling average for the squeeze filter) ----
    std::deque<double> m_atr_history;
    double m_atr_avg = 0.0;

    // ---- Donchian channel ----
    std::deque<double> m_highs, m_lows;

    // ---- Signal state ----
    bool   m_signal_pending = false;
    bool   m_signal_long    = false;
    double m_signal_atr     = 0.0;

    // ---- Cooldown ----
    int64_t m_cooldown_until = 0;

    // ---- Consecutive S63 loss tracking ----
    int     m_consec_loss_cut = 0;
    int64_t m_consec_block_until = 0;

    // ==========================================================================
    // Bar close: update indicators, check entry signal
    // ==========================================================================
    void _on_bar_close(const M5Bar& bar) {
        if (!m_ema_inited) {
            m_ema9.init(9);
            m_ema21.init(21);
            m_ema_inited = true;
        }
        m_ema9.push(bar.close);
        m_ema21.push(bar.close);
        m_atr.push(bar.high, bar.low, bar.close);

        // ATR-of-ATR rolling average for squeeze filter
        if (m_atr.primed) {
            m_atr_history.push_back(m_atr.value);
            if ((int)m_atr_history.size() > ATR_AVG_LOOKBACK) m_atr_history.pop_front();
            double s = 0.0; for (auto v : m_atr_history) s += v;
            m_atr_avg = m_atr_history.empty() ? 0.0 : s / m_atr_history.size();
        }

        // Donchian channel
        m_highs.push_back(bar.high);
        m_lows.push_back(bar.low);
        if ((int)m_highs.size() > LOOKBACK + 1) { m_highs.pop_front(); m_lows.pop_front(); }

        m_signal_pending = false;

        // Readiness gates
        if (!m_atr.primed) return;
        if (!m_ema9.primed || !m_ema21.primed) return;
        if ((int)m_highs.size() <= LOOKBACK) return;
        if (m_pos.active) return;

        // Weekend gate
        if (_is_weekend(bar.ts_open)) return;
        // ---- NEW FILTER #1: session window (NY-only by default) ----
        if (!_is_session_active(bar.ts_open)) return;

        // ATR floor/cap
        if (m_atr.value < ATR_FLOOR || m_atr.value > ATR_CAP) return;

        // Donchian breakout over prior N bars (exclude current)
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

        // EMA trend alignment
        if (intend_long  && m_ema9.value <= m_ema21.value) return;
        if (!intend_long && m_ema9.value >= m_ema21.value) return;

        // Momentum bar filter
        const double body  = std::fabs(bar.close - bar.open);
        const double range = bar.high - bar.low;
        if (range < 0.01) return;
        if (body / range < 0.40) return;
        const double mid_price = (bar.high + bar.low) * 0.5;
        if (intend_long  && bar.close < mid_price) return;
        if (!intend_long && bar.close > mid_price) return;

        // Signal valid -- store for next tick's on_tick to consume
        m_signal_pending = true;
        m_signal_long    = intend_long;
        m_signal_atr     = m_atr.value;
    }

    // ==========================================================================
    // Manage position: per-tick SL/TP/trail + pyramid + S63
    // ==========================================================================
    void _manage_position(double bid, double ask, int64_t now_ms) {
        if (!m_pos.active) return;

        const double move = m_pos.is_long
            ? (bid - m_pos.base_entry)
            : (m_pos.base_entry - ask);
        const double adverse = -move;

        if (move > m_pos.mfe_peak) {
            m_pos.mfe_peak  = move;
            m_pos.mfe_price = m_pos.is_long ? bid : ask;
        }
        if (adverse > m_pos.mae) m_pos.mae = adverse;

        // ---- S63 LOSS_CUT ----
        if (LOSS_CUT_PCT > 0.0 && now_ms >= m_consec_block_until) {
            const double loss_cut_dist = m_pos.base_entry * LOSS_CUT_PCT / 100.0;
            if (adverse >= loss_cut_dist) {
                _close_position(bid, ask, now_ms, "LOSS_CUT");
                m_cooldown_until = now_ms + COOLDOWN_SEC * 1000LL;
                ++m_consec_loss_cut;
                if (m_consec_loss_cut >= 2) {
                    m_consec_block_until = now_ms + 30LL * 60 * 1000LL;
                    m_consec_loss_cut = 0;
                }
                return;
            }
        }

        // ---- S63 BE_RATCHET ----
        if (BE_ARM_PCT > 0.0 && BE_BUFFER_PCT >= 0.0) {
            const double arm_pts    = m_pos.base_entry * BE_ARM_PCT / 100.0;
            const double buffer_pts = m_pos.base_entry * BE_BUFFER_PCT / 100.0;
            if (m_pos.mfe_peak >= arm_pts && move <= buffer_pts) {
                _close_position(bid, ask, now_ms, "BE_CUT");
                m_cooldown_until = now_ms + COOLDOWN_SEC * 1000LL;
                m_consec_loss_cut = 0;
                return;
            }
        }

        // ---- 4-phase trail (mirror GSP) ----
        double new_trail = m_pos.hard_sl;

        // Phase 1: BE lock at MFE >= cost*2.5
        if (m_pos.mfe_peak >= COST_RT_PTS * 2.5) {
            const double be = m_pos.is_long
                ? (m_pos.base_entry + COST_RT_PTS)
                : (m_pos.base_entry - COST_RT_PTS);
            if (m_pos.is_long) new_trail = std::max(new_trail, be);
            else               new_trail = std::min(new_trail, be);
        }

        // Phase 2: lock 35% of MFE
        if (m_pos.mfe_peak >= m_pos.atr_at_entry * 0.4) {
            const double lock = m_pos.mfe_peak * 0.35;
            const double lv = m_pos.is_long
                ? (m_pos.base_entry + lock)
                : (m_pos.base_entry - lock);
            if (m_pos.is_long) new_trail = std::max(new_trail, lv);
            else               new_trail = std::min(new_trail, lv);
        }

        // Phase 3: tight trail behind MFE
        if (m_pos.mfe_peak >= m_pos.atr_at_entry * 0.7) {
            const double td = m_pos.atr_at_entry * TRAIL_TIGHT;
            const double tl = m_pos.is_long
                ? (m_pos.mfe_price - td)
                : (m_pos.mfe_price + td);
            if (m_pos.is_long) new_trail = std::max(new_trail, tl);
            else               new_trail = std::min(new_trail, tl);
        }

        // Phase 4: very tight at MFE >= 1.2 * ATR
        if (m_pos.mfe_peak >= m_pos.atr_at_entry * 1.2) {
            const double td = m_pos.atr_at_entry * TRAIL_TIGHT * 0.60;
            const double tl = m_pos.is_long
                ? (m_pos.mfe_price - td)
                : (m_pos.mfe_price + td);
            if (m_pos.is_long) new_trail = std::max(new_trail, tl);
            else               new_trail = std::min(new_trail, tl);
        }

        // Ratchet
        if (m_pos.is_long) m_pos.trail_sl = std::max(m_pos.trail_sl, new_trail);
        else               m_pos.trail_sl = std::min(m_pos.trail_sl, new_trail);

        // ---- Exits ----
        const double eff_sl = m_pos.is_long
            ? std::max(m_pos.hard_sl, m_pos.trail_sl)
            : std::min(m_pos.hard_sl, m_pos.trail_sl);

        if (m_pos.is_long && bid <= eff_sl) {
            const char* reason = (m_pos.trail_sl > m_pos.hard_sl) ? "TRAIL_HIT" : "SL_HIT";
            _close_position(bid, ask, now_ms, reason);
            m_cooldown_until = now_ms + COOLDOWN_SEC * 1000LL;
            if (std::string(reason) == "SL_HIT") m_consec_loss_cut = 0;
            return;
        }
        if (!m_pos.is_long && ask >= eff_sl) {
            const char* reason = (m_pos.trail_sl < m_pos.hard_sl) ? "TRAIL_HIT" : "SL_HIT";
            _close_position(bid, ask, now_ms, reason);
            m_cooldown_until = now_ms + COOLDOWN_SEC * 1000LL;
            if (std::string(reason) == "SL_HIT") m_consec_loss_cut = 0;
            return;
        }

        // TP
        if (m_pos.is_long && bid >= m_pos.hard_tp) {
            _close_position(bid, ask, now_ms, "TP_HIT");
            m_cooldown_until = now_ms + COOLDOWN_SEC * 1000LL;
            m_consec_loss_cut = 0;
            return;
        }
        if (!m_pos.is_long && ask <= m_pos.hard_tp) {
            _close_position(bid, ask, now_ms, "TP_HIT");
            m_cooldown_until = now_ms + COOLDOWN_SEC * 1000LL;
            m_consec_loss_cut = 0;
            return;
        }

        // Time stop
        const int bars_held = (int)(m_bars_seen - m_pos.entry_bar_seq);
        if (bars_held >= MAX_HOLD_BARS) {
            _close_position(bid, ask, now_ms, "TIME_STOP");
            m_cooldown_until = now_ms + COOLDOWN_SEC * 1000LL;
            return;
        }

        // ---- Pyramid adds (GSP shape) ----
        if (PYRAMID_ON && m_pos.next_pyramid_idx < MAX_LAYERS) {
            static const double pyr_thresh[]    = {0.0, 1.0, 1.5, 2.0, 3.0};
            static const double pyr_size_mult[] = {1.0, 0.80, 0.65, 0.50, 0.40};

            const double sl_dist = m_pos.atr_at_entry * SL_ATR_MULT;
            const int    idx     = m_pos.next_pyramid_idx;
            const double threshold = pyr_thresh[idx] * sl_dist;

            if (m_pos.mfe_peak >= threshold) {
                const double base_size = m_pos.layers[0].size;
                double add_size = base_size * pyr_size_mult[idx];
                add_size = std::max(LOT_MIN, std::min(LOT_MAX, add_size));
                add_size = std::floor(add_size / 0.01) * 0.01;

                const double add_entry = m_pos.is_long ? ask : bid;
                m_pos.layers[idx]   = {true, add_entry, add_size};
                m_pos.n_layers      = idx + 1;
                m_pos.next_pyramid_idx = idx + 1;

                char buf[320];
                std::snprintf(buf, sizeof(buf),
                    "[GSB] PYRAMID L%d %s entry=%.2f size=%.3f mfe=%.2f shadow=%s\n",
                    idx + 1, m_pos.is_long ? "LONG" : "SHORT",
                    add_entry, add_size, m_pos.mfe_peak,
                    shadow_mode ? "true" : "false");
                std::printf("%s", buf);
                std::fflush(stdout);
            }
        }
    }

    // ==========================================================================
    // Close all layers, fire TradeRecord
    //
    // S99h+S99j conventions: tr.pnl/mfe/mae in pts*lots, populate spreadAtEntry.
    // ==========================================================================
    void _close_position(double bid, double ask, int64_t now_ms, const char* reason)
    {
        const double exit_px = m_pos.is_long ? bid : ask;

        double total_pnl_pts_lots = 0.0;
        double total_size_lots    = 0.0;
        for (int i = 0; i < MAX_LAYERS; ++i) {
            if (!m_pos.layers[i].active) continue;
            const double layer_move = m_pos.is_long
                ? (exit_px - m_pos.layers[i].entry)
                : (m_pos.layers[i].entry - exit_px);
            total_pnl_pts_lots += layer_move * m_pos.layers[i].size;
            total_size_lots    += m_pos.layers[i].size;
        }
        const double total_pnl_usd = total_pnl_pts_lots * USD_PER_PT_LOT;

        char buf[512];
        std::snprintf(buf, sizeof(buf),
            "[GSB] CLOSE %s entry=%.2f exit=%.2f pnl=$%.2f size=%.3f layers=%d "
            "mfe=%.2f mae=%.2f bars=%d reason=%s shadow=%s\n",
            m_pos.is_long ? "LONG" : "SHORT",
            m_pos.weighted_entry(), exit_px, total_pnl_usd, total_size_lots,
            m_pos.n_layers, m_pos.mfe_peak, m_pos.mae,
            (int)(m_bars_seen - m_pos.entry_bar_seq), reason,
            shadow_mode ? "true" : "false");
        std::printf("%s", buf);
        std::fflush(stdout);

        omega::TradeRecord tr;
        tr.engine        = "GoldSessionBreakout";
        tr.symbol        = "XAUUSD";
        tr.side          = m_pos.is_long ? "LONG" : "SHORT";
        tr.regime        = "M5_BREAKOUT_NY";
        tr.entryPrice    = m_pos.weighted_entry();
        tr.exitPrice     = exit_px;
        tr.tp            = m_pos.hard_tp;
        tr.sl            = m_pos.hard_sl;
        tr.size          = total_size_lots;
        tr.pnl           = total_pnl_pts_lots;             // pts*lots; downstream * 100 -> USD
        tr.entryTs       = m_pos.entry_ts / 1000LL;
        tr.exitTs        = now_ms / 1000LL;
        tr.exitReason    = reason;
        tr.mfe           = m_pos.mfe_peak * total_size_lots;  // pts*lots
        tr.mae           = m_pos.mae      * total_size_lots;  // pts*lots
        tr.spreadAtEntry = m_pos.spread_at_entry;             // needed by apply_realistic_costs
        tr.atr_at_entry  = m_pos.atr_at_entry;
        tr.shadow        = shadow_mode;

        if (on_close_cb) on_close_cb(tr);

        m_pos = LivePos{};
    }

    // ---- Time helpers --------------------------------------------------------
    static bool _is_weekend(int64_t ts_ms) noexcept {
        const int64_t s   = ts_ms / 1000LL;
        const int     dow = static_cast<int>((s / 86400LL + 3) % 7);
        const int     hr  = static_cast<int>((s % 86400LL) / 3600LL);
        if (dow == 4 && hr >= 20) return true;
        if (dow == 5) return true;
        if (dow == 6 && hr < 22) return true;
        return false;
    }

    bool _is_session_active(int64_t ts_ms) const noexcept {
        const int64_t s  = ts_ms / 1000LL;
        const int     hr = static_cast<int>((s % 86400LL) / 3600LL);
        // Inclusive start, exclusive end. Handles wrap (e.g. 22 -> 4) too.
        if (SESSION_START_HOUR_UTC <= SESSION_END_HOUR_UTC) {
            return (hr >= SESSION_START_HOUR_UTC && hr < SESSION_END_HOUR_UTC);
        } else {
            return (hr >= SESSION_START_HOUR_UTC || hr < SESSION_END_HOUR_UTC);
        }
    }
};

}  // namespace omega
