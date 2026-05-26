#pragma once
// =============================================================================
// FxScalpPyramidEngine.hpp -- M5 FX Scalp with Pyramid + Aggressive Trail
// =============================================================================
//
// 2026-05-26 S38d (Claude / Jo):
//   1:1 clone of GoldScalpPyramidEngine.hpp adapted for FX pairs.
//   Same M5 Donchian + EMA + ADX + cost-aware BE + pyramid + trail design.
//
//   PRIMARY DIFFERENCES vs GSP:
//     * Per-pair runtime-tunable constants (set via set_pair_config()):
//         USD_PER_PT_LOT, HALF_SPREAD, ATR_FLOOR, ATR_CAP, SPREAD_CAP,
//         COST_RT_PTS, decimals
//     * Momentum bar filter range threshold rescaled from hardcoded "0.01"
//       (gold cents) to "ATR_FLOOR * 0.1" (per-pair). Without this every
//       FX bar gets blocked because gold's $0.01 is enormous in FX units.
//     * Log namespace [GSP] -> [FSP].
//     * tr.engine / tr.regime parameterised per instance.
//     * tr.symbol from instance config (was hardcoded "XAUUSD" on GSP).
//
//   VALIDATION (backtest/fx_scalp_pyramid_bt.cpp 13mo standalone harness):
//     EURUSD: +$1,808 / PF 1.56  (best)
//     USDJPY: +$1,688 / PF 1.51
//     GBPUSD: +$1,207 / PF 1.32
//     USDCAD: +$506   / PF 1.23
//     AUDUSD: +$410   / PF 1.23
//     SKIPPED: NZDUSD (PF 1.07 marginal), EURGBP (PF 0.93 negative).
//
// SAFETY:
//   shadow_mode = true by default. 14-day shadow validation required
//   before any live promotion (same gate as GSP S38c re-enable).
//
// LOG NAMESPACE:
//   All log lines use prefix [FSP].
//   tr.engine set per instance, e.g. "FxScalpPyramid_EURUSD".
//   tr.regime = "FX_M5_SCALP".
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
#include "OHLCBarEngine.hpp"  // for OHLCBar struct used by prime_from_history()

namespace omega {

// Reuse ExitPhilosophy enum defined in GoldScalpPyramidEngine.hpp via include
// chain.  If GoldScalpPyramidEngine.hpp is NOT included in the same TU we
// fall through to the local definition below.
#ifndef OMEGA_EXIT_PHILOSOPHY_DEFINED
#define OMEGA_EXIT_PHILOSOPHY_DEFINED
// Forward-define the enum mirror only when GSP header hasn't been included.
// Using inline-namespace style is overkill; just guard against double-defn.
#endif

class FxScalpPyramidEngine {
public:
    // ---- Timing ---------------------------------------------------------------
    static constexpr int BAR_SECS = 300;  // 5 minutes

    // ---- Donchian lookback (tunable via engine_init.hpp) ----------------------
    int LOOKBACK = 12;

    // ---- Risk parameters (tunable via engine_init.hpp) ------------------------
    double SL_ATR_MULT   = 1.0;
    double TP_ATR_MULT   = 2.0;
    double TRAIL_TIGHT   = 0.20;

    // ---- Pyramid config -------------------------------------------------------
    bool   PYRAMID_ON    = true;
    static constexpr int MAX_LAYERS = 5;

    // ---- S63 in-flight protection (VWR pattern) --------------------------------
    double LOSS_CUT_PCT  = 0.05;
    double BE_ARM_PCT    = 0.03;
    double BE_BUFFER_PCT = 0.012;

    // ---- L2 tuning (FX usually has degraded/synthetic L2 -- thresholds inert
    //      when l2_real=false, but kept for parity with GSP) ---------------------
    double L2_IMBAL_LONG_MIN  = 0.58;
    double L2_IMBAL_SHORT_MAX = 0.42;
    double L2_SLOPE_CONFIRM   = 0.10;
    double L2_TRAIL_TIGHTEN   = 0.60;
    double L2_SIZE_BOOST      = 1.20;
    double L2_SIZE_WALL_CUT   = 0.70;
    double L2_PYRAMID_ACCEL   = 0.80;

    // ---- Sizing (USD_PER_PT_LOT is now runtime-tunable per pair) ---------------
    double USD_PER_PT_LOT = 100000.0;   // default majors xxxUSD
    static constexpr double RISK_DOLLARS = 50.0;
    static constexpr double LOT_MIN      = 0.01;
    static constexpr double LOT_MAX      = 0.05;

    // ---- Cost model (tunable per pair via set_pair_config) ---------------------
    double COST_RT_PTS        = 0.00040;
    double BE_ARM_COST_MULT   = 2.0;
    double HALF_SPREAD        = 0.00002;

    // ---- Chop / whipsaw filter ------------------------------------------------
    double CHOP_ER_MIN        = 0.0;   // S38b default: ER disabled
    int    CHOP_ER_LOOKBACK   = 10;
    int    CONSEC_BE_FREEZE_N = 3;

    // ---- ADX directional-strength filter --------------------------------------
    double CHOP_ADX_MIN       = 10.0;  // S38c default: free DD reduction
    int    CHOP_ADX_PERIOD    = 14;

    // ---- Range-expansion filter -----------------------------------------------
    double RANGE_EXP_MULT     = 0.0;
    int    RANGE_EXP_LB       = 10;

    // ---- Session / filter constants (per-pair, runtime-tunable) ---------------
    double ATR_FLOOR  = 0.00030;
    double ATR_CAP    = 0.00500;
    double SPREAD_CAP = 0.00050;
    int    DECIMALS   = 5;            // for log formatting (5 for majors, 3 for JPY)
    static constexpr int    MAX_HOLD_BARS  = 12;
    static constexpr int    COOLDOWN_SEC   = 60;

    // ---- Per-instance identity ------------------------------------------------
    std::string symbol;        // e.g. "EURUSD" -- set via set_pair_config
    std::string engine_name;   // e.g. "FxScalpPyramid_EURUSD" -- auto-built

    // ---- Public state ---------------------------------------------------------
    bool shadow_mode = true;
    bool enabled     = true;

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;
    CloseCallback on_close_cb;
    std::function<void(const std::string&)> cancel_fn;

    // =========================================================================
    // Set all per-pair constants in one call. Called once at startup from
    // engine_init.hpp. engine_name is auto-built as "FxScalpPyramid_<sym>".
    // =========================================================================
    void set_pair_config(const std::string& sym,
                         double cost,
                         double half_spread,
                         double usd_per_pt,
                         double atr_floor,
                         double atr_cap,
                         double spread_cap,
                         int    decimals)
    {
        symbol         = sym;
        engine_name    = std::string("FxScalpPyramid_") + sym;
        COST_RT_PTS    = cost;
        HALF_SPREAD    = half_spread;
        USD_PER_PT_LOT = usd_per_pt;
        ATR_FLOOR      = atr_floor;
        ATR_CAP        = atr_cap;
        SPREAD_CAP     = spread_cap;
        DECIMALS       = decimals;
    }

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
        double  mfe_peak     = 0.0;
        double  mfe_price    = 0.0;
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

    // ---- Public interface: feed every FX tick ---------------------------------
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
            _manage_position(bid, ask, now_ms,
                             l2_imbalance, book_slope,
                             vacuum_ask, vacuum_bid,
                             wall_above, wall_below, l2_real,
                             ext_close);
            return;
        }

        // ----- Entry check (only on bar-close ticks) -----
        if (!m_signal_pending) return;
        m_signal_pending = false;

        if (!can_enter) return;
        if (now_ms < m_cooldown_until) return;

        if (spread > SPREAD_CAP) return;

        // Cost gate (pass instance symbol so OmegaCostGuard uses correct pip math)
        double tp_dist = m_signal_atr * TP_ATR_MULT;
        if (!::ExecutionCostGuard::is_viable(symbol.c_str(), spread, tp_dist, LOT_MIN, 1.5)) return;

        // ---- L2 ENTRY FILTERS ----
        if (l2_real) {
            if (m_signal_long && l2_imbalance < L2_IMBAL_SHORT_MAX) return;
            if (!m_signal_long && l2_imbalance > L2_IMBAL_LONG_MIN) return;
            if (m_signal_long && wall_above) return;
            if (!m_signal_long && wall_below) return;
        }

        // Open position
        double sl_dist  = m_signal_atr * SL_ATR_MULT;
        double entry_px = m_signal_long ? ask : bid;
        double sl_px    = m_signal_long ? (entry_px - sl_dist) : (entry_px + sl_dist);
        double tp_px    = m_signal_long ? (entry_px + tp_dist) : (entry_px - tp_dist);

        // ---- Lot sizing ----
        double size = RISK_DOLLARS / (sl_dist * USD_PER_PT_LOT);
        if (l2_real) {
            bool slope_confirms = (m_signal_long && book_slope > L2_SLOPE_CONFIRM)
                               || (!m_signal_long && book_slope < -L2_SLOPE_CONFIRM);
            if (slope_confirms) size *= L2_SIZE_BOOST;
            bool wall_against = (m_signal_long && wall_above) || (!m_signal_long && wall_below);
            if (wall_against) size *= L2_SIZE_WALL_CUT;
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

        char buf[512];
        snprintf(buf, sizeof(buf),
            "[FSP] %s OPEN %s entry=%.*f sl=%.*f tp=%.*f size=%.3f atr=%.*f spread=%.*f "
            "l2_imb=%.3f slope=%.3f l2=%s shadow=%s\n",
            engine_name.c_str(),
            m_signal_long ? "LONG" : "SHORT",
            DECIMALS, entry_px, DECIMALS, sl_px, DECIMALS, tp_px,
            size, DECIMALS, m_signal_atr, DECIMALS, spread,
            l2_imbalance, book_slope,
            l2_real ? "live" : "stale",
            shadow_mode ? "true" : "false");
        std::printf("%s", buf);
        std::fflush(stdout);
    }

    // =========================================================================
    // Prime EMA9/EMA21/ATR14 directly from persisted atomic indicator values.
    // FX side currently has no pre-populated bar history; this entry point
    // exists for forward-compat (when FX M5 bar source is wired).
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
        std::printf("[FSP-PRIME-ATOMICS] %s ema9=%.*f ema21=%.*f atr14=%.*f last_close=%.*f "
                    "seeded=%d\n",
                    engine_name.c_str(),
                    DECIMALS, ema9_val, DECIMALS, ema21_val,
                    DECIMALS, atr14_val, DECIMALS, last_close,
                    (int)seeded_any);
        std::fflush(stdout);
    }

    int prime_from_history(const std::deque<OHLCBar>& bars) noexcept {
        if (bars.empty()) {
            std::printf("[FSP-WARMUP] %s no bars to feed -- cold start\n",
                        engine_name.c_str());
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
        m_signal_pending = false;
        m_cur_anchor     = -1;
        std::printf("[FSP-WARMUP] %s fed=%d M5 bars ema9=%d ema21=%d atr=%d donch=%d bars_seen=%lld\n",
                    engine_name.c_str(), fed,
                    (int)m_ema9.primed, (int)m_ema21.primed,
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
    std::deque<double> m_closes;
    std::deque<double> m_ranges;
    int                m_consec_be_cut = 0;

    // Wilder ADX state
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

    int     m_consec_loss_cut = 0;
    int64_t m_consec_block_until = 0;

    // ---- Warmup state ----
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

        m_highs.push_back(bar.high);
        m_lows.push_back(bar.low);
        m_closes.push_back(bar.close);
        m_ranges.push_back(bar.high - bar.low);
        m_adx.push(bar.high, bar.low, bar.close, CHOP_ADX_PERIOD);
        const int win = std::max(LOOKBACK + 1,
                       std::max(CHOP_ER_LOOKBACK + 1, RANGE_EXP_LB + 1));
        if ((int)m_highs.size()  > win) { m_highs.pop_front();  m_lows.pop_front(); }
        if ((int)m_closes.size() > win)   m_closes.pop_front();
        if ((int)m_ranges.size() > win)   m_ranges.pop_front();

        if (m_pos.active) {
            m_pos.bars_held = (int)(m_bars_seen - m_pos.entry_bar_seq);
        }

        m_signal_pending = false;
        if (m_in_warmup) return;

        if (!m_atr.primed) return;
        if (!m_ema9.primed || !m_ema21.primed) return;
        if ((int)m_highs.size() <= LOOKBACK) return;
        if (m_pos.active) return;

        if (_is_weekend(bar.ts_open)) return;
        if (!_is_session_active(bar.ts_open)) return;

        if (m_atr.value < ATR_FLOOR || m_atr.value > ATR_CAP) return;

        if (CHOP_ADX_MIN > 0.0 && m_adx.primed_adx && m_adx.adx_value < CHOP_ADX_MIN) {
            return;
        }

        if (RANGE_EXP_MULT > 0.0 && (int)m_ranges.size() >= RANGE_EXP_LB + 1) {
            const int end = (int)m_ranges.size() - 1;
            double sum_r = 0.0;
            for (int k = end - RANGE_EXP_LB; k < end; ++k) sum_r += m_ranges[k];
            double avg_r = sum_r / RANGE_EXP_LB;
            if (avg_r > 1e-9 && m_ranges[end] / avg_r < RANGE_EXP_MULT) return;
        }

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

        if (CONSEC_BE_FREEZE_N > 0 && m_consec_be_cut >= CONSEC_BE_FREEZE_N) {
            return;
        }

        double ch_high = -1e18, ch_low = 1e18;
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

        if (intend_long  && m_ema9.value <= m_ema21.value) return;
        if (!intend_long && m_ema9.value >= m_ema21.value) return;

        // Momentum bar filter -- S38d FX-fix: range threshold scales with
        // ATR_FLOOR so it's per-pair-correct. GSP hardcoded 0.01 (gold cents);
        // for FX majors (price ~1.0) that blocks 99.99% of bars.
        double body  = std::fabs(bar.close - bar.open);
        double range = bar.high - bar.low;
        const double min_range = ATR_FLOOR * 0.1;
        if (range < min_range) return;
        if (body / range < 0.40) return;
        double mid_price = (bar.high + bar.low) * 0.5;
        if (intend_long  && bar.close < mid_price) return;
        if (!intend_long && bar.close > mid_price) return;

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

        if (move > m_pos.mfe_peak) {
            m_pos.mfe_peak  = move;
            m_pos.mfe_price = m_pos.is_long ? bid : ask;
        }
        if (adverse > m_pos.mae) m_pos.mae = adverse;

        // S63 LOSS_CUT
        if (LOSS_CUT_PCT > 0.0 && now_ms >= m_consec_block_until) {
            double loss_cut_dist = m_pos.base_entry * LOSS_CUT_PCT / 100.0;
            if (adverse >= loss_cut_dist) {
                _close_position(bid, ask, now_ms, "LOSS_CUT", ext_close);
                m_cooldown_until = now_ms + COOLDOWN_SEC * 1000LL;
                ++m_consec_loss_cut;
                if (m_consec_loss_cut >= 2) {
                    m_consec_block_until = now_ms + 30LL * 60 * 1000LL;
                    m_consec_loss_cut = 0;
                }
                return;
            }
        }

        // S63 BE_RATCHET
        if (BE_ARM_PCT > 0.0 && BE_BUFFER_PCT >= 0.0) {
            double arm_pts    = m_pos.base_entry * BE_ARM_PCT / 100.0;
            double buffer_pts = m_pos.base_entry * BE_BUFFER_PCT / 100.0;
            if (m_pos.mfe_peak >= arm_pts && move <= buffer_pts) {
                _close_position(bid, ask, now_ms, "BE_CUT", ext_close);
                m_cooldown_until = now_ms + COOLDOWN_SEC * 1000LL;
                m_consec_loss_cut = 0;
                ++m_consec_be_cut;
                if (m_consec_be_cut >= CONSEC_BE_FREEZE_N) {
                    m_consec_block_until = now_ms + 30LL * 60 * 1000LL;
                }
                return;
            }
        }

        // L2-adaptive trail factor
        double trail_mult = 1.0;
        if (l2_real) {
            bool l2_against = (m_pos.is_long && l2_imbalance < L2_IMBAL_SHORT_MAX)
                           || (!m_pos.is_long && l2_imbalance > L2_IMBAL_LONG_MIN);
            bool slope_against = (m_pos.is_long && book_slope < -L2_SLOPE_CONFIRM)
                              || (!m_pos.is_long && book_slope > L2_SLOPE_CONFIRM);
            if (l2_against && slope_against) {
                trail_mult = L2_TRAIL_TIGHTEN;
            }
        }

        // 4-phase aggressive trailing stop
        double new_trail = m_pos.hard_sl;

        if (m_pos.mfe_peak >= COST_RT_PTS * BE_ARM_COST_MULT) {
            double be = m_pos.is_long
                ? (m_pos.base_entry + COST_RT_PTS)
                : (m_pos.base_entry - COST_RT_PTS);
            if (m_pos.is_long) new_trail = std::max(new_trail, be);
            else               new_trail = std::min(new_trail, be);
        }

        if (m_pos.mfe_peak >= m_pos.atr_at_entry * 0.4) {
            double lock = m_pos.mfe_peak * 0.35;
            double lv = m_pos.is_long
                ? (m_pos.base_entry + lock)
                : (m_pos.base_entry - lock);
            if (m_pos.is_long) new_trail = std::max(new_trail, lv);
            else               new_trail = std::min(new_trail, lv);
        }

        if (m_pos.mfe_peak >= m_pos.atr_at_entry * 0.7) {
            double td = m_pos.atr_at_entry * TRAIL_TIGHT * trail_mult;
            double tl = m_pos.is_long
                ? (m_pos.mfe_price - td)
                : (m_pos.mfe_price + td);
            if (m_pos.is_long) new_trail = std::max(new_trail, tl);
            else               new_trail = std::min(new_trail, tl);
        }

        if (m_pos.mfe_peak >= m_pos.atr_at_entry * 1.2) {
            double td = m_pos.atr_at_entry * TRAIL_TIGHT * 0.60 * trail_mult;
            double tl = m_pos.is_long
                ? (m_pos.mfe_price - td)
                : (m_pos.mfe_price + td);
            if (m_pos.is_long) new_trail = std::max(new_trail, tl);
            else               new_trail = std::min(new_trail, tl);
        }

        if (m_pos.is_long) m_pos.trail_sl = std::max(m_pos.trail_sl, new_trail);
        else               m_pos.trail_sl = std::min(m_pos.trail_sl, new_trail);

        double eff_sl = m_pos.is_long
            ? std::max(m_pos.hard_sl, m_pos.trail_sl)
            : std::min(m_pos.hard_sl, m_pos.trail_sl);

        if (m_pos.is_long && bid <= eff_sl) {
            const char* reason = (m_pos.trail_sl > m_pos.hard_sl) ? "TRAIL_HIT" : "SL_HIT";
            _close_position(bid, ask, now_ms, reason, ext_close);
            m_cooldown_until = now_ms + COOLDOWN_SEC * 1000LL;
            if (std::string(reason) == "SL_HIT") m_consec_loss_cut = 0;
            if (std::string(reason) == "TRAIL_HIT") m_consec_be_cut = 0;
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

        if (m_pos.is_long && bid >= m_pos.hard_tp) {
            _close_position(bid, ask, now_ms, "TP_HIT", ext_close);
            m_cooldown_until = now_ms + COOLDOWN_SEC * 1000LL;
            m_consec_loss_cut = 0;
            m_consec_be_cut   = 0;
            return;
        }
        if (!m_pos.is_long && ask <= m_pos.hard_tp) {
            _close_position(bid, ask, now_ms, "TP_HIT", ext_close);
            m_cooldown_until = now_ms + COOLDOWN_SEC * 1000LL;
            m_consec_loss_cut = 0;
            m_consec_be_cut   = 0;
            return;
        }

        int bars_held = (int)(m_bars_seen - m_pos.entry_bar_seq);
        if (bars_held >= MAX_HOLD_BARS) {
            _close_position(bid, ask, now_ms, "TIME_STOP", ext_close);
            m_cooldown_until = now_ms + COOLDOWN_SEC * 1000LL;
            return;
        }

        // Pyramid adds (L2-gated)
        if (PYRAMID_ON && m_pos.next_pyramid_idx < MAX_LAYERS) {
            static const double pyr_thresh[] = {0.0, 1.0, 1.5, 2.0, 3.0};
            static const double pyr_size_mult[] = {1.0, 0.80, 0.65, 0.50, 0.40};

            double sl_dist = m_pos.atr_at_entry * SL_ATR_MULT;
            int idx = m_pos.next_pyramid_idx;
            double threshold = pyr_thresh[idx] * sl_dist;

            if (l2_real) {
                bool slope_confirms = (m_pos.is_long && book_slope > L2_SLOPE_CONFIRM)
                                   || (!m_pos.is_long && book_slope < -L2_SLOPE_CONFIRM);
                bool vacuum_with = (m_pos.is_long && vacuum_ask)
                                || (!m_pos.is_long && vacuum_bid);

                if (slope_confirms && vacuum_with) {
                    threshold *= L2_PYRAMID_ACCEL;
                }

                bool wall_against = (m_pos.is_long && wall_above)
                                 || (!m_pos.is_long && wall_below);
                if (wall_against) {
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

                char buf[512];
                snprintf(buf, sizeof(buf),
                    "[FSP] %s PYRAMID L%d %s entry=%.*f size=%.3f mfe=%.*f "
                    "l2_imb=%.3f slope=%.3f shadow=%s\n",
                    engine_name.c_str(),
                    idx + 1, m_pos.is_long ? "LONG" : "SHORT",
                    DECIMALS, add_entry, add_size,
                    DECIMALS, m_pos.mfe_peak,
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

        char buf[640];
        snprintf(buf, sizeof(buf),
            "[FSP] %s CLOSE %s entry=%.*f exit=%.*f pnl=$%.2f size=%.3f layers=%d "
            "mfe=%.*f mae=%.*f bars=%d reason=%s shadow=%s\n",
            engine_name.c_str(),
            m_pos.is_long ? "LONG" : "SHORT",
            DECIMALS, m_pos.weighted_entry(), DECIMALS, exit_px,
            total_pnl_usd, total_size,
            m_pos.n_layers,
            DECIMALS, m_pos.mfe_peak, DECIMALS, m_pos.mae,
            (int)(m_bars_seen - m_pos.entry_bar_seq), reason,
            shadow_mode ? "true" : "false");
        std::printf("%s", buf);
        std::fflush(stdout);

        omega::TradeRecord tr;
        tr.engine     = engine_name;
        tr.symbol     = symbol;
        tr.side       = m_pos.is_long ? "LONG" : "SHORT";
        tr.regime     = "FX_M5_SCALP";
        tr.entryPrice = m_pos.weighted_entry();
        tr.exitPrice  = exit_px;
        tr.size       = total_size;
        tr.pnl        = total_pnl_pts_lots;
        tr.entryTs    = m_pos.entry_ts / 1000LL;
        tr.exitTs     = now_ms / 1000LL;
        tr.exitReason = reason;
        tr.mfe        = m_pos.mfe_peak * total_size;
        tr.mae        = m_pos.mae      * total_size;
        tr.spreadAtEntry = m_pos.spread_at_entry;
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
