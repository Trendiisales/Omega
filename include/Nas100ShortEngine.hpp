#pragma once
// =============================================================================
//  Nas100ShortEngine.hpp -- NAS100/USTEC short-only trend engine
//                           S94 2026-05-15
// =============================================================================
//
//  PROVENANCE
//  ----------
//  Built from Nas100UltimateBacktest.cpp v1→v4b iterative discovery on 830
//  days of USTEC tick data (NSXUSD_merged.csv, 125M ticks, Mar 2024 → Dec
//  2025). Edge validated with 60/40 IS/OOS split.
//
//  v4b final metrics (full tape, 44 trades over 830 days):
//      PF=2.47  WR=59.1%  PnL=$214.86  maxDD=$42.57  Sharpe=30.05
//
//  OOS validation (last 40%, 332 days, 21 trades):
//      PF=3.38  WR=66.7%  PnL=$129.89  maxDD=$22.19  Sharpe=39.85
//      PF decay IS→OOS: NEGATIVE (OOS outperforms IS — no overfit signal)
//
//  STRATEGY
//  --------
//  Short-only momentum-following entry during US session hours when:
//    - ATR(14) in 30-60 pt band (moderate volatility, not chop or spike)
//    - 50-bar cumulative drift < -100 pts (strong downward momentum)
//    - EMA-9 < EMA-50 (trend alignment)
//    - RSI-14 in 30-62 (not oversold)
//    - Price < EMA-9 (momentum confirmation)
//    - Not overextended from EMA-50 (< 3.0*ATR distance)
//    - Session hours: 14,16,17,18,20 UTC (US session)
//
//  Geometry: SL=2.0*ATR, TP=4.0*ATR, trail at 3.0 ATR MFE / 2.0 ATR dist.
//
//  SAFETY
//  ------
//    - shadow_mode = true by default (HARD shadow).
//    - 0.1 lot (USTEC pt_size = 0.1 → $0.10/pt at 0.1 lot).
//    - Max 1 concurrent position.
//    - Spread cap = 5.0 pts.
//    - S63 LOSS_CUT / BE_RATCHET wired and active from day 1.
//    - 5-minute cooldown between evaluations.
//
//  USAGE
//  -----
//      // globals.hpp:
//      static omega::Nas100ShortEngine g_nas100_short;
//
//      // engine_init.hpp:
//      g_nas100_short.shadow_mode = true;
//      g_nas100_short.enabled     = true;
//      g_nas100_short.lot         = 0.1;
//      g_nas100_short.max_spread  = 5.0;
//      g_nas100_short.init();
//      g_nas100_short.on_fire_hook = [](int64_t now_s) {
//          g_risk_monitor.on_fire("Nas100Short", now_s);
//      };
//
//      // tick_indices.hpp every USTEC tick:
//      g_nas100_short.on_tick(bid, ask, now_ms, ca_on_close);
// =============================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <string>

#include "OmegaTradeLedger.hpp"
#include "OmegaCostGuard.hpp"

namespace omega {

// =============================================================================
// Position state for the single short position
// =============================================================================
struct Nas100ShortPos {
    bool        active         = false;
    bool        is_long        = false;   // always false for this engine
    double      entry_px       = 0.0;
    double      tp_px          = 0.0;
    double      sl_px          = 0.0;
    double      atr_at_entry   = 0.0;
    int64_t     entry_ts_ms    = 0;
    double      mfe_pts        = 0.0;
    double      mae_pts        = 0.0;
    bool        trail_active   = false;
    std::string broker_position_id;
    std::string entry_clOrdId;
};

// =============================================================================
// 1-minute bar (internal aggregation from ticks)
// =============================================================================
struct Nas100Bar1m {
    int64_t open_ms    = 0;
    double  open       = 0.0;
    double  high       = 0.0;
    double  low        = 99999999.0;
    double  close      = 0.0;
    int     tick_count = 0;

    void reset(int64_t ts, double price) noexcept {
        open_ms    = ts;
        open       = price;
        high       = price;
        low        = price;
        close      = price;
        tick_count = 1;
    }

    void update(double price) noexcept {
        if (price > high) high = price;
        if (price < low)  low  = price;
        close = price;
        tick_count++;
    }
};

// =============================================================================
// Nas100ShortEngine
// =============================================================================
struct Nas100ShortEngine {
public:
    // ── Config (set from engine_init.hpp) ────────────────────────────────────
    bool   shadow_mode = true;
    bool   enabled     = false;
    double lot         = 0.1;
    double max_spread  = 5.0;

    // ── S63 in-flight protection (wired from day 1) ─────────────────────────
    // USTEC.F ~21,000 in mid-2025 range.
    //   LOSS_CUT_PCT  = 0.10 → ~21 pt cold-loss cut (within SL range)
    //   BE_ARM_PCT    = 0.06 → ~12.6 pt arms the BE ratchet (MFE validated)
    //   BE_BUFFER_PCT = 0.02 → ~4.2 pt buffer (slightly above spread)
    double LOSS_CUT_PCT  = 0.10;
    double BE_ARM_PCT    = 0.06;
    double BE_BUFFER_PCT = 0.02;

    // ── RiskMonitor hook ────────────────────────────────────────────────────
    std::function<void(int64_t now_s)> on_fire_hook;

    // ── Strategy constants (from v4b backtest) ──────────────────────────────
    // All derived from 830-day backtest with OOS validation.
    static constexpr double SL_ATR_MULT      = 2.0;
    static constexpr double TP_ATR_MULT      = 4.0;
    static constexpr double TRAIL_TRIGGER_ATR = 3.0;  // trail arms at 3.0*ATR MFE
    static constexpr double TRAIL_DIST_ATR    = 2.0;  // trail distance = 2.0*ATR

    static constexpr double DRIFT_ENTRY_MIN  = 100.0; // 50-bar cumulative drift minimum
    static constexpr double ATR_ENTRY_FLOOR  = 30.0;  // ATR(14) 1-min must be >= 30
    static constexpr double ATR_ENTRY_CEIL   = 60.0;  // ATR(14) 1-min must be <= 60

    static constexpr double RSI_SHORT_MIN    = 30.0;  // RSI floor (not oversold)
    static constexpr double RSI_SHORT_MAX    = 62.0;  // RSI ceiling
    static constexpr double EMA_DIST_MAX_ATR = 3.0;   // max distance from EMA-50

    static constexpr int64_t EVAL_INTERVAL_MS = 300000; // 5-min between evaluations
    static constexpr int     WARMUP_BARS      = 55;

    // ── Session hours (UTC) — v4b validated ─────────────────────────────────
    // 14=US open, 16=US midday, 17=US afternoon, 18=US late, 20=US close
    static bool session_ok(int hour_utc) noexcept {
        return (hour_utc == 14 || hour_utc == 16 || hour_utc == 17 ||
                hour_utc == 18 || hour_utc == 20);
    }

    // ── State ───────────────────────────────────────────────────────────────
    Nas100ShortPos pos{};

    using OnCloseFn = std::function<void(const omega::TradeRecord&)>;

    // ── Indicators (1-min bar-based) ────────────────────────────────────────
    static constexpr int64_t BAR_PERIOD_MS     = 60000;
    static constexpr int     ATR_PERIOD        = 14;
    static constexpr int     RSI_PERIOD        = 14;
    static constexpr int     DRIFT_WINDOW      = 50;
    static constexpr int     DRIFT_LONG_WINDOW = 200;
    static constexpr int     BAR_HISTORY       = 260;

    Nas100Bar1m             current_bar_{};
    bool                    bar_active_   = false;
    std::deque<Nas100Bar1m> bars_;
    int                     total_bars_   = 0;

    // Indicator values
    double atr_pts_        = 80.0;
    double ewm_drift_      = 0.0;
    double ewm_drift_long_ = 0.0;
    double rsi14_          = 50.0;
    double ema9_           = 0.0;
    double ema50_          = 0.0;

    // RSI internals
    double avg_gain_ = 0.0;
    double avg_loss_ = 0.0;

    // ATR internals
    std::deque<double> tr_history_;

    // Drift internals
    std::deque<double> bar_returns_;

    // Eval timing
    int64_t last_eval_ms_ = 0;

    // ═════════════════════════════════════════════════════════════════════════
    // PUBLIC INTERFACE
    // ═════════════════════════════════════════════════════════════════════════

    void init() noexcept {
        bars_.clear();
        tr_history_.clear();
        bar_returns_.clear();
        total_bars_    = 0;
        bar_active_    = false;
        atr_pts_       = 80.0;
        ewm_drift_     = 0.0;
        ewm_drift_long_= 0.0;
        rsi14_         = 50.0;
        ema9_          = 0.0;
        ema50_         = 0.0;
        avg_gain_      = 0.0;
        avg_loss_      = 0.0;
        last_eval_ms_  = 0;
        pos = Nas100ShortPos{};

        std::printf("[NAS100-SHORT] init: shadow=%s enabled=%s lot=%.2f "
                    "spread_cap=%.1f LC=%.2f%% BE=%.2f%%/%.2f%%\n",
                    shadow_mode ? "true" : "false",
                    enabled ? "true" : "false",
                    lot, max_spread,
                    LOSS_CUT_PCT, BE_ARM_PCT, BE_BUFFER_PCT);
        std::fflush(stdout);
    }

    void on_tick(double bid, double ask, int64_t now_ms,
                 OnCloseFn on_close) noexcept {
        if (!enabled) return;

        const double mid = (bid + ask) * 0.5;
        if (mid < 1000.0 || mid > 100000.0) return; // sanity

        // Weekend gate
        if (_is_weekend(now_ms)) return;

        // Aggregate into 1-min bars
        _feed_tick(mid, now_ms);

        if (total_bars_ < WARMUP_BARS) return;

        // ── Manage open position ─────────────────────────────────────────
        if (pos.active) {
            _manage_open(bid, ask, now_ms, on_close);
            return;
        }

        // ── Evaluate new entry ───────────────────────────────────────────
        if (now_ms - last_eval_ms_ < EVAL_INTERVAL_MS) return;
        last_eval_ms_ = now_ms;

        // Session filter
        const int hour_utc = static_cast<int>((now_ms / 1000 % 86400) / 3600);
        if (!session_ok(hour_utc)) return;

        // ATR band
        if (atr_pts_ < ATR_ENTRY_FLOOR || atr_pts_ > ATR_ENTRY_CEIL) return;

        // Short-only: drift must be negative and strong
        if (ewm_drift_ > -DRIFT_ENTRY_MIN) return;

        // Quality filter
        if (!_short_entry_ok()) return;

        // Spread gate
        const double spread = ask - bid;
        if (spread > max_spread) return;

        // Cost gate
        const double tp_dist = atr_pts_ * TP_ATR_MULT;
        if (!ExecutionCostGuard::is_viable("USTEC.F", spread, tp_dist, lot, 1.5))
            return;

        // ── FIRE SHORT ───────────────────────────────────────────────────
        _fire_entry(bid, ask, now_ms);

        std::printf("[NAS100-SHORT] ENTRY SHORT @ %.1f sl=%.1f tp=%.1f "
                    "atr=%.1f drift=%.1f rsi=%.1f hour=%d %s\n",
                    pos.entry_px, pos.sl_px, pos.tp_px,
                    pos.atr_at_entry, ewm_drift_, rsi14_, hour_utc,
                    shadow_mode ? "[SHADOW]" : "[LIVE]");
        std::fflush(stdout);

        if (on_fire_hook) on_fire_hook(now_ms / 1000);
    }

    void force_close(double bid, double ask, int64_t now_ms,
                     OnCloseFn on_close, const char* reason = nullptr) noexcept {
        if (!pos.active) return;
        double xp = pos.is_long ? bid : ask;
        _close(xp, reason ? reason : "FORCE_CLOSE", now_ms, on_close);
    }

private:
    // ═════════════════════════════════════════════════════════════════════════
    // TICK → BAR AGGREGATION
    // ═════════════════════════════════════════════════════════════════════════

    void _feed_tick(double mid, int64_t ts_ms) noexcept {
        if (!bar_active_) {
            current_bar_.reset(ts_ms, mid);
            bar_active_ = true;
            return;
        }

        if (ts_ms - current_bar_.open_ms >= BAR_PERIOD_MS) {
            bars_.push_back(current_bar_);
            total_bars_++;
            _on_bar_close();
            current_bar_.reset(ts_ms, mid);
            while ((int)bars_.size() > BAR_HISTORY) bars_.pop_front();
            return;
        }

        current_bar_.update(mid);
    }

    // ═════════════════════════════════════════════════════════════════════════
    // INDICATOR UPDATES (on each 1-min bar close)
    // ═════════════════════════════════════════════════════════════════════════

    void _on_bar_close() noexcept {
        if (bars_.size() < 2) return;

        const auto& bar  = bars_.back();
        const auto& prev = bars_[bars_.size() - 2];

        // True Range & ATR
        const double tr = std::max({
            bar.high - bar.low,
            std::fabs(bar.high - prev.close),
            std::fabs(bar.low - prev.close)
        });
        tr_history_.push_back(tr);
        if ((int)tr_history_.size() > ATR_PERIOD) tr_history_.pop_front();

        if ((int)tr_history_.size() >= ATR_PERIOD) {
            double sum = 0.0;
            for (double t : tr_history_) sum += t;
            atr_pts_ = sum / ATR_PERIOD;
        } else {
            double sum = 0.0;
            for (double t : tr_history_) sum += t;
            atr_pts_ = sum / tr_history_.size();
        }
        atr_pts_ = std::max(10.0, atr_pts_); // clamp minimum

        // Drift (dual-timeframe cumulative)
        const double bar_return = bar.close - prev.close;
        bar_returns_.push_back(bar_return);
        if ((int)bar_returns_.size() > DRIFT_LONG_WINDOW) bar_returns_.pop_front();

        ewm_drift_ = 0.0;
        int short_count = 0;
        for (int i = (int)bar_returns_.size() - 1;
             i >= 0 && short_count < DRIFT_WINDOW; --i, ++short_count) {
            ewm_drift_ += bar_returns_[i];
        }

        ewm_drift_long_ = 0.0;
        for (double r : bar_returns_) ewm_drift_long_ += r;

        // RSI-14
        const double change = bar.close - prev.close;
        const double gain = (change > 0) ? change : 0.0;
        const double loss = (change < 0) ? -change : 0.0;

        if (total_bars_ <= RSI_PERIOD + 1) {
            avg_gain_ += gain / RSI_PERIOD;
            avg_loss_ += loss / RSI_PERIOD;
            rsi14_ = 50.0;
        } else {
            avg_gain_ = (avg_gain_ * (RSI_PERIOD - 1) + gain) / RSI_PERIOD;
            avg_loss_ = (avg_loss_ * (RSI_PERIOD - 1) + loss) / RSI_PERIOD;
            if (avg_loss_ > 0.0001) {
                rsi14_ = 100.0 - 100.0 / (1.0 + avg_gain_ / avg_loss_);
            } else {
                rsi14_ = 100.0;
            }
        }

        // EMAs
        const double k9  = 2.0 / (9.0 + 1.0);
        const double k50 = 2.0 / (50.0 + 1.0);
        if (ema9_ == 0.0)  ema9_  = bar.close;
        if (ema50_ == 0.0) ema50_ = bar.close;
        ema9_  = bar.close * k9  + ema9_  * (1.0 - k9);
        ema50_ = bar.close * k50 + ema50_ * (1.0 - k50);
    }

    // ═════════════════════════════════════════════════════════════════════════
    // ENTRY FILTER — short-only, all conditions from v4b
    // ═════════════════════════════════════════════════════════════════════════

    bool _short_entry_ok() const noexcept {
        // EMA alignment: EMA-9 below EMA-50 (bearish)
        if (!(ema9_ < ema50_)) return false;

        // RSI not oversold / not overbought
        if (rsi14_ < RSI_SHORT_MIN || rsi14_ > RSI_SHORT_MAX) return false;

        // Long-term drift must agree (negative for shorts)
        if (ewm_drift_long_ > 0.0) return false;

        // Price below EMA-9 (momentum confirmation)
        if (!bars_.empty()) {
            const double close = bars_.back().close;
            if (close > ema9_) return false;

            // Not overextended from EMA-50
            const double dist = std::fabs(close - ema50_);
            if (dist > atr_pts_ * EMA_DIST_MAX_ATR) return false;
        }

        return true;
    }

    // ═════════════════════════════════════════════════════════════════════════
    // FIRE ENTRY
    // ═════════════════════════════════════════════════════════════════════════

    void _fire_entry(double bid, double ask, int64_t now_ms) noexcept {
        const double sl_dist = atr_pts_ * SL_ATR_MULT;
        const double tp_dist = atr_pts_ * TP_ATR_MULT;

        pos.active       = true;
        pos.is_long      = false;  // always short
        pos.entry_px     = bid;    // short fills at bid
        pos.sl_px        = pos.entry_px + sl_dist;
        pos.tp_px        = pos.entry_px - tp_dist;
        pos.atr_at_entry = atr_pts_;
        pos.entry_ts_ms  = now_ms;
        pos.mfe_pts      = 0.0;
        pos.mae_pts      = 0.0;
        pos.trail_active = false;
        pos.broker_position_id.clear();
        pos.entry_clOrdId.clear();
    }

    // ═════════════════════════════════════════════════════════════════════════
    // MANAGE OPEN POSITION (per-tick)
    // ═════════════════════════════════════════════════════════════════════════

    void _manage_open(double bid, double ask, int64_t now_ms,
                      OnCloseFn on_close) noexcept {
        const double mid = (bid + ask) * 0.5;

        // MFE/MAE update (short: favourable = entry - mid)
        const double favourable = pos.entry_px - mid;
        if (favourable > pos.mfe_pts) pos.mfe_pts = favourable;
        if (favourable < pos.mae_pts) pos.mae_pts = favourable;

        // ── S63 in-flight protection ─────────────────────────────────────
        const double move    = pos.entry_px - mid;  // positive = our way
        const double adverse = -move;

        // Phase 1: BE ratchet
        if (BE_ARM_PCT > 0.0 && BE_BUFFER_PCT >= 0.0 && pos.entry_px > 0.0) {
            const double arm_pts    = pos.entry_px * BE_ARM_PCT    / 100.0;
            const double buffer_pts = pos.entry_px * BE_BUFFER_PCT / 100.0;
            if (pos.mfe_pts >= arm_pts && move <= buffer_pts) {
                std::printf("[NAS100-SHORT] BE_CUT mfe=%.2f arm=%.2f "
                            "move=%.2f buf=%.2f %s\n",
                            pos.mfe_pts, arm_pts, move, buffer_pts,
                            shadow_mode ? "[SHADOW]" : "[LIVE]");
                std::fflush(stdout);
                _close(ask, "BE_CUT", now_ms, on_close);
                return;
            }
        }

        // Phase 2: cold loss cut
        if (LOSS_CUT_PCT > 0.0 && pos.entry_px > 0.0) {
            const double loss_cut_dist = pos.entry_px * LOSS_CUT_PCT / 100.0;
            if (adverse >= loss_cut_dist) {
                std::printf("[NAS100-SHORT] LOSS_CUT adverse=%.2f >= %.2f %s\n",
                            adverse, loss_cut_dist,
                            shadow_mode ? "[SHADOW]" : "[LIVE]");
                std::fflush(stdout);
                _close(ask, "LOSS_CUT", now_ms, on_close);
                return;
            }
        }

        // ── Trail stop ───────────────────────────────────────────────────
        if (pos.mfe_pts >= pos.atr_at_entry * TRAIL_TRIGGER_ATR) {
            pos.trail_active = true;
            const double trail_distance = pos.atr_at_entry * TRAIL_DIST_ATR;
            const double trail_level = pos.mfe_pts - trail_distance;
            if (trail_level > 0.0) {
                // Short: trail SL moves DOWN (tightens)
                const double trail_sl = pos.entry_px - trail_level;
                if (trail_sl < pos.sl_px) pos.sl_px = trail_sl;
            }
        }

        // ── SL / TP check ────────────────────────────────────────────────
        // Short: SL hit when ask >= sl_px, TP hit when ask <= tp_px
        bool hit_sl = (ask >= pos.sl_px);
        bool hit_tp = (ask <= pos.tp_px);

        if (hit_sl || hit_tp) {
            const double xp = hit_tp ? pos.tp_px : pos.sl_px;
            const char* reason = hit_tp ? "TP_HIT" :
                                 (pos.trail_active ? "TRAIL_EXIT" : "SL_HIT");
            _close(xp, reason, now_ms, on_close);
        }
    }

    // ═════════════════════════════════════════════════════════════════════════
    // CLOSE
    // ═════════════════════════════════════════════════════════════════════════

    void _close(double exit_px, const char* reason,
                int64_t now_ms, OnCloseFn on_close) noexcept {
        if (!pos.active) return;

        // PnL: short → entry - exit, in raw pts * lots
        const double pts_move = pos.entry_px - exit_px;

        omega::TradeRecord tr;
        tr.symbol     = "USTEC.F";
        tr.engine     = "Nas100Short";
        tr.side       = "SHORT";
        tr.entryPrice = pos.entry_px;
        tr.exitPrice  = exit_px;
        tr.tp         = pos.tp_px;
        tr.sl         = pos.sl_px;
        tr.size       = lot;
        tr.pnl        = pts_move * lot;
        tr.net_pnl    = tr.pnl;          // trade_lifecycle overwrites after costs
        tr.mfe        = pos.mfe_pts * lot;
        tr.mae        = pos.mae_pts * lot;
        tr.entryTs    = pos.entry_ts_ms / 1000;
        tr.exitTs     = now_ms / 1000;
        tr.exitReason = reason;
        tr.regime     = "v4b_short_only";
        tr.shadow     = shadow_mode;

        std::printf("[NAS100-SHORT] CLOSE %s exit=%.1f pnl=%.2f mfe=%.1f "
                    "mae=%.1f atr=%.1f %s %s\n",
                    reason, exit_px, tr.pnl, pos.mfe_pts, pos.mae_pts,
                    pos.atr_at_entry,
                    shadow_mode ? "[SHADOW]" : "[LIVE]",
                    tr.pnl > 0 ? "WIN" : "LOSS");
        std::fflush(stdout);

        pos.active = false;
        pos = Nas100ShortPos{};

        if (on_close) on_close(tr);
    }

    // ═════════════════════════════════════════════════════════════════════════
    // WEEKEND GATE
    // ═════════════════════════════════════════════════════════════════════════

    static bool _is_weekend(int64_t ts_ms) noexcept {
        const int64_t sec = ts_ms / 1000;
        const int dow = ((sec / 86400) + 4) % 7;  // 0=Sun, 1=Mon, ..., 5=Fri, 6=Sat
        if (dow == 0 || dow == 6) return true;
        if (dow == 5) {
            const int hour = static_cast<int>((sec % 86400) / 3600);
            if (hour >= 21) return true;
        }
        return false;
    }
};

} // namespace omega
