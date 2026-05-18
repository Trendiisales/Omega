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

namespace omega {

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

    // ---- Cost model -----------------------------------------------------------
    static constexpr double COST_RT_PTS    = 0.50;
    static constexpr double HALF_SPREAD    = 0.25;

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

    // ---- Signal state ----
    bool   m_signal_pending = false;
    bool   m_signal_long    = false;
    double m_signal_atr     = 0.0;

    // ---- Cooldown ----
    int64_t m_cooldown_until = 0;

    // ---- Consecutive S63 loss tracking ----
    int     m_consec_loss_cut = 0;
    int64_t m_consec_block_until = 0;

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
        if ((int)m_highs.size() > LOOKBACK + 1) { m_highs.pop_front(); m_lows.pop_front(); }

        // Update bar counter for open position
        if (m_pos.active) {
            m_pos.bars_held = (int)(m_bars_seen - m_pos.entry_bar_seq);
        }

        // ---- Entry signal check ----
        m_signal_pending = false;

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

        // ---- 4-phase aggressive trailing stop ----
        double new_trail = m_pos.hard_sl;

        // Phase 1: BE lock at MFE >= cost * 2.5
        if (m_pos.mfe_peak >= COST_RT_PTS * 2.5) {
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
            return;
        }
        if (!m_pos.is_long && ask >= eff_sl) {
            const char* reason = (m_pos.trail_sl < m_pos.hard_sl) ? "TRAIL_HIT" : "SL_HIT";
            _close_position(bid, ask, now_ms, reason, ext_close);
            m_cooldown_until = now_ms + COOLDOWN_SEC * 1000LL;
            if (std::string(reason) == "SL_HIT") m_consec_loss_cut = 0;
            return;
        }

        // TP hit
        if (m_pos.is_long && bid >= m_pos.hard_tp) {
            _close_position(bid, ask, now_ms, "TP_HIT", ext_close);
            m_cooldown_until = now_ms + COOLDOWN_SEC * 1000LL;
            m_consec_loss_cut = 0;
            return;
        }
        if (!m_pos.is_long && ask <= m_pos.hard_tp) {
            _close_position(bid, ask, now_ms, "TP_HIT", ext_close);
            m_cooldown_until = now_ms + COOLDOWN_SEC * 1000LL;
            m_consec_loss_cut = 0;
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
    // =========================================================================
    void _close_position(double bid, double ask, int64_t now_ms,
                         const char* reason,
                         const CloseCallback* ext_close)
    {
        double exit_px = m_pos.is_long ? bid : ask;

        // Compute total PnL across all layers
        double total_pnl = 0.0;
        double total_size = 0.0;
        for (int i = 0; i < m_pos.n_layers; ++i) {
            if (!m_pos.layers[i].active) continue;
            double layer_move = m_pos.is_long
                ? (exit_px - m_pos.layers[i].entry)
                : (m_pos.layers[i].entry - exit_px);
            total_pnl  += layer_move * m_pos.layers[i].size * USD_PER_PT_LOT;
            total_size += m_pos.layers[i].size;
        }

        char buf[512];
        snprintf(buf, sizeof(buf),
            "[GSP] CLOSE %s entry=%.2f exit=%.2f pnl=$%.2f size=%.3f layers=%d "
            "mfe=%.2f mae=%.2f bars=%d reason=%s shadow=%s\n",
            m_pos.is_long ? "LONG" : "SHORT",
            m_pos.weighted_entry(), exit_px, total_pnl, total_size,
            m_pos.n_layers, m_pos.mfe_peak, m_pos.mae,
            (int)(m_bars_seen - m_pos.entry_bar_seq), reason,
            shadow_mode ? "true" : "false");
        std::printf("%s", buf);
        std::fflush(stdout);

        // Fire TradeRecord
        omega::TradeRecord tr;
        tr.engine     = "GoldScalpPyramid";
        tr.symbol     = "XAUUSD";
        tr.side       = m_pos.is_long ? "LONG" : "SHORT";
        tr.regime     = "M5_SCALP";
        tr.entryPrice = m_pos.weighted_entry();
        tr.exitPrice  = exit_px;
        tr.size       = total_size;
        tr.pnl        = total_pnl;
        tr.entryTs    = m_pos.entry_ts / 1000LL;  // TradeRecord uses unix seconds
        tr.exitTs     = now_ms / 1000LL;
        tr.exitReason = reason;
        tr.mfe        = m_pos.mfe_peak;
        tr.mae        = m_pos.mae;
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
