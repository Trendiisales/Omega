#pragma once
// =============================================================================
// GoldUltimateEngine.hpp -- Standalone XAUUSD trend engine (v12 OOS-validated)
// =============================================================================
//
// PROVENANCE
//
// Built from GoldUltimateBacktest.cpp v12 signal logic. 26-month backtest
// on 154M XAUUSD ticks (Mar 2024 – Apr 2026):
//
//     PF=1.36, WR=41.8%, Sharpe=8.30, 311 trades
//     BULL PF=1.45, BEAR PF=1.29
//     OOS PF=1.39 (265 trades), 117% PF retention
//
// SIGNAL LOGIC (7-factor entry filter):
//   1. EMA9 vs EMA50 alignment
//   2. RSI not exhausted (long: 38-70, short: 30-62)
//   3. Drift >= 2.0 in trade direction (50-bar cumulative)
//   4. Long-term drift agrees (200-bar cumulative, same sign)
//   5. ATR >= 2.5 (v12 raised from 1.5; ATR 1.5-2.5 had PF=0.80)
//   6. Pullback entry (within 3.0 ATR of EMA50)
//   7. Momentum confirmation (price on correct side of EMA9)
//
// EDGE FILTERS:
//   - Edge hours: 01, 05, 23 UTC only (OOS-confirmed)
//   - ATR floor: 2.5 (low-vol band removed)
//
// GEOMETRY:
//   - SL = 2.0 ATR, TP = 5.0 ATR
//   - Trail: trigger at 3.0 ATR MFE, distance 2.0 ATR from peak
//   - No break-even, no profit lock, no adaptive SL tightening
//
// ARCHITECTURE:
//   Self-contained tick-driven engine. Aggregates 1-minute bars internally,
//   computes all indicators, generates entry signals, manages open position.
//   Single position at a time. Follows XauTrendFollow2hEngine dispatch pattern.
//
// USAGE:
//   // globals.hpp:
//   static omega::GoldUltimateEngine g_gold_ultimate_engine;
//
//   // engine_init.hpp:
//   g_gold_ultimate_engine.shadow_mode = true;  // or false for live
//   g_gold_ultimate_engine.enabled = true;
//   g_gold_ultimate_engine.lot = 0.01;
//   g_gold_ultimate_engine.max_spread = 1.0;
//
//   // tick_gold.hpp (per-tick):
//   g_gold_ultimate_engine.on_tick(bid, ask, now_ms_g, ca_on_close);
// =============================================================================

#include <cstdint>
#include <cmath>
#include <cstdio>
#include <deque>
#include <array>
#include <string>
#include <functional>
#include <algorithm>
#include "OmegaTradeLedger.hpp"
#include "OmegaCostGuard.hpp"

namespace omega {

struct GoldUltimateEngine {
public:
    // ── Configuration ──────────────────────────────────────────────────────
    bool   shadow_mode = true;
    bool   enabled     = false;
    double lot         = 0.01;
    double max_spread  = 1.0;

    // v12 edge filters (configurable, defaults from backtest evidence)
    std::array<bool, 24> edge_hours = []() {
        std::array<bool, 24> h{};
        h[1]  = true;  // 01:00 UTC — IS PF=1.44, OOS PF=1.44
        h[5]  = true;  // 05:00 UTC — OOS PF=1.50 (70 trades)
        h[23] = true;  // 23:00 UTC — OOS PF=1.22 (73 trades)
        return h;
    }();
    double atr_entry_floor = 2.5;  // ATR must be >= 2.5 for entry

    // Geometry
    double sl_atr_mult      = 2.0;
    double tp_atr_mult      = 5.0;
    double trail_trigger_atr = 3.0;  // MFE needed to activate trail
    double trail_dist_atr    = 2.0;  // trail distance behind peak

    // Entry filter thresholds
    double drift_min        = 2.0;   // |drift| >= 2.0 for entry
    double rsi_long_lo      = 38.0;
    double rsi_long_hi      = 70.0;
    double rsi_short_lo     = 30.0;
    double rsi_short_hi     = 62.0;
    double pullback_max_atr = 3.0;   // max dist from EMA50 in ATR units

    using OnCloseFn = std::function<void(const TradeRecord&)>;

    // ── Bar ──────────────────────────────────────────────────────────────
    struct Bar1m {
        int64_t open_ms  = 0;
        double  open     = 0.0;
        double  high     = 0.0;
        double  low      = 99999.0;
        double  close    = 0.0;
        int     tick_count = 0;

        void reset(int64_t ts, double price) noexcept {
            open_ms = ts; open = high = low = close = price; tick_count = 1;
        }
        void update(double price) noexcept {
            if (price > high) high = price;
            if (price < low)  low  = price;
            close = price;
            tick_count++;
        }
    };

    // ── Position ────────────────────────────────────────────────────────
    struct Pos {
        bool    active       = false;
        bool    is_long      = false;
        double  entry_px     = 0.0;
        double  sl_px        = 0.0;
        double  tp_px        = 0.0;
        double  atr_at_entry = 0.0;
        int64_t entry_ts_ms  = 0;
        double  mfe          = 0.0;
        double  mae          = 0.0;
        bool    trail_active = false;
        double  trail_peak   = 0.0;  // best price in trade direction
    };

    Pos pos{};

    // ── Indicators ──────────────────────────────────────────────────────
    static constexpr int64_t BAR_PERIOD_MS    = 60000;   // 1-minute bars
    static constexpr int     ATR_PERIOD       = 14;
    static constexpr int     RSI_PERIOD       = 14;
    static constexpr int     DRIFT_WINDOW     = 50;
    static constexpr int     DRIFT_LONG_WINDOW = 200;
    static constexpr int     STRUCTURE_WINDOW = 20;
    static constexpr int     WARMUP_BARS      = 55;      // EMA50 stabilises ~50 bars; drift/RSI/ATR work on partial windows

    Bar1m              cur_bar_{};
    bool               bar_active_ = false;
    int                total_bars_  = 0;
    std::deque<Bar1m>  bars_;

    // Indicator state
    double atr_pts_       = 3.0;
    double ewm_drift_     = 0.0;
    double ewm_drift_long_ = 0.0;
    double rsi14_         = 50.0;
    double vol_ratio_     = 1.0;
    double ema9_          = 0.0;
    double ema50_         = 0.0;
    bool   higher_highs_  = false;
    bool   lower_lows_    = false;

    // RSI internals
    double rsi_avg_gain_  = 0.0;
    double rsi_avg_loss_  = 0.0;

    // ATR internals
    std::deque<double> tr_history_;

    // Drift internals
    std::deque<double> bar_returns_;

    // Vol internals
    double recent_vol_ewm_ = 3.0;
    double base_vol_ewm_   = 3.0;

    // Entry eval interval (don't evaluate every tick)
    static constexpr int64_t EVAL_INTERVAL_MS = 5000;  // 5 seconds
    int64_t last_eval_ms_ = 0;

    // ── Public interface ────────────────────────────────────────────────

    void init() noexcept {
        bars_.clear();
        cur_bar_ = {};
        bar_active_ = false;
        total_bars_ = 0;
        atr_pts_ = 3.0;
        ewm_drift_ = 0.0;
        ewm_drift_long_ = 0.0;
        rsi14_ = 50.0;
        vol_ratio_ = 1.0;
        ema9_ = 0.0;
        ema50_ = 0.0;
        higher_highs_ = false;
        lower_lows_ = false;
        rsi_avg_gain_ = 0.0;
        rsi_avg_loss_ = 0.0;
        tr_history_.clear();
        bar_returns_.clear();
        recent_vol_ewm_ = 3.0;
        base_vol_ewm_ = 3.0;
        last_eval_ms_ = 0;
        pos = {};
    }

    bool has_open_position() const noexcept { return pos.active; }

    void on_tick(double bid, double ask, int64_t now_ms,
                 OnCloseFn on_close) noexcept {
        if (!enabled) return;
        if (bid <= 0 || ask <= 0 || ask < bid) return;

        const double mid = (bid + ask) * 0.5;

        // ── Feed tick to 1-minute bar aggregator ────────────────────────
        _feed_tick(mid, now_ms);

        // Skip warmup
        if (total_bars_ < WARMUP_BARS) return;

        // ── Manage open position ────────────────────────────────────────
        if (pos.active) {
            _manage_open(bid, ask, now_ms, on_close);
            return;  // don't evaluate new entry while position open
        }

        // ── Evaluate new entry (throttled) ──────────────────────────────
        if (now_ms - last_eval_ms_ < EVAL_INTERVAL_MS) return;
        last_eval_ms_ = now_ms;

        _evaluate_entry(bid, ask, now_ms, on_close);
    }

    void force_close(double bid, double ask, int64_t now_ms,
                     OnCloseFn on_close, const char* reason = "FORCE_CLOSE") noexcept {
        if (!pos.active) return;
        double xp = pos.is_long ? bid : ask;
        _close(xp, reason, now_ms, on_close);
    }

private:
    // ── Bar aggregation ─────────────────────────────────────────────────
    void _feed_tick(double mid, int64_t ts_ms) noexcept {
        if (!bar_active_) {
            cur_bar_.reset(ts_ms, mid);
            bar_active_ = true;
            return;
        }

        if (ts_ms - cur_bar_.open_ms >= BAR_PERIOD_MS) {
            // Close current bar
            bars_.push_back(cur_bar_);
            total_bars_++;
            _on_bar_close();

            // Start new bar
            cur_bar_.reset(ts_ms, mid);

            // Keep bounded
            while (bars_.size() > 260) bars_.pop_front();
            return;
        }

        cur_bar_.update(mid);
    }

    // ── Indicator computation (on each 1-min bar close) ─────────────────
    void _on_bar_close() noexcept {
        if (bars_.size() < 2) return;

        const auto& bar  = bars_.back();
        const auto& prev = bars_[bars_.size() - 2];

        // True Range & ATR
        const double tr = std::max({
            bar.high - bar.low,
            std::fabs(bar.high - prev.close),
            std::fabs(bar.low  - prev.close)
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
        atr_pts_ = std::max(0.5, atr_pts_);

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

        // RSI
        const double change = bar.close - prev.close;
        const double gain = (change > 0) ? change : 0.0;
        const double loss = (change < 0) ? -change : 0.0;

        if (total_bars_ <= RSI_PERIOD + 1) {
            rsi_avg_gain_ += gain / RSI_PERIOD;
            rsi_avg_loss_ += loss / RSI_PERIOD;
            rsi14_ = 50.0;
        } else {
            rsi_avg_gain_ = (rsi_avg_gain_ * (RSI_PERIOD - 1) + gain) / RSI_PERIOD;
            rsi_avg_loss_ = (rsi_avg_loss_ * (RSI_PERIOD - 1) + loss) / RSI_PERIOD;
            if (rsi_avg_loss_ > 0.0001) {
                const double rs = rsi_avg_gain_ / rsi_avg_loss_;
                rsi14_ = 100.0 - 100.0 / (1.0 + rs);
            } else {
                rsi14_ = 100.0;
            }
        }

        // EMAs
        const double k9  = 2.0 / (9.0 + 1.0);
        const double k50 = 2.0 / (50.0 + 1.0);
        if (ema9_  == 0.0) ema9_  = bar.close;
        if (ema50_ == 0.0) ema50_ = bar.close;
        ema9_  = bar.close * k9  + ema9_  * (1.0 - k9);
        ema50_ = bar.close * k50 + ema50_ * (1.0 - k50);

        // Vol ratio
        recent_vol_ewm_ = 0.90 * recent_vol_ewm_ + 0.10 * tr;
        base_vol_ewm_   = 0.99 * base_vol_ewm_   + 0.01 * tr;
        vol_ratio_ = (base_vol_ewm_ > 0.01) ? recent_vol_ewm_ / base_vol_ewm_ : 1.0;

        // Structure (higher highs / lower lows)
        if ((int)bars_.size() >= STRUCTURE_WINDOW) {
            const int n = std::min((int)bars_.size(), STRUCTURE_WINDOW);
            const int half = n / 2;

            double recent_high = 0.0, older_high = 0.0;
            double recent_low = 99999.0, older_low = 99999.0;

            for (int i = 0; i < half; ++i) {
                const auto& b = bars_[bars_.size() - 1 - i];
                if (b.high > recent_high) recent_high = b.high;
                if (b.low < recent_low)   recent_low  = b.low;
            }
            for (int i = half; i < n; ++i) {
                const auto& b = bars_[bars_.size() - 1 - i];
                if (b.high > older_high) older_high = b.high;
                if (b.low < older_low)   older_low  = b.low;
            }

            higher_highs_ = (recent_high > older_high);
            lower_lows_   = (recent_low < older_low);
        }
    }

    // ── Entry evaluation ────────────────────────────────────────────────
    void _evaluate_entry(double bid, double ask, int64_t now_ms,
                         OnCloseFn /*on_close*/) noexcept {
        // Edge-hour filter
        const int hour_utc = static_cast<int>((now_ms / 1000 % 86400) / 3600);
        if (hour_utc < 0 || hour_utc >= 24 || !edge_hours[hour_utc]) return;

        // Spread check
        const double spread = ask - bid;
        if (spread > max_spread) return;

        // Determine candidate direction from drift
        const bool candidate_long = (ewm_drift_ > 0.0);

        // 7-factor entry filter
        if (!_trend_entry_ok(candidate_long)) return;

        // Dual-timeframe direction veto
        if (candidate_long && ewm_drift_long_ < -3.0) return;
        if (!candidate_long && ewm_drift_long_ > 3.0) return;

        // SL/TP
        const double sl_pts = atr_pts_ * sl_atr_mult;
        const double tp_pts = atr_pts_ * tp_atr_mult;

        // Cost guard
        if (!ExecutionCostGuard::is_viable("XAUUSD", spread, tp_pts, lot, 1.5)) return;

        // Fire entry
        _fire_entry(candidate_long, bid, ask, now_ms, sl_pts, tp_pts);
    }

    bool _trend_entry_ok(bool is_long) const noexcept {
        // 1. EMA alignment
        if (is_long  && !(ema9_ > ema50_)) return false;
        if (!is_long && !(ema9_ < ema50_)) return false;

        // 2. RSI not exhausted
        if (is_long  && (rsi14_ > rsi_long_hi  || rsi14_ < rsi_long_lo))  return false;
        if (!is_long && (rsi14_ < rsi_short_lo  || rsi14_ > rsi_short_hi)) return false;

        // 3. Drift in trade direction
        if (is_long  && ewm_drift_ < drift_min) return false;
        if (!is_long && ewm_drift_ > -drift_min) return false;

        // 4. Long-term drift agrees
        if (is_long  && ewm_drift_long_ < 0.0) return false;
        if (!is_long && ewm_drift_long_ > 0.0) return false;

        // 5. ATR floor
        if (atr_pts_ < atr_entry_floor) return false;

        // 6. Pullback entry (not chasing)
        if (!bars_.empty()) {
            const double close = bars_.back().close;
            const double dist_to_ema50 = std::fabs(close - ema50_);
            if (dist_to_ema50 > atr_pts_ * pullback_max_atr) return false;
        }

        // 7. Momentum confirmation (price on correct side of EMA9)
        if (!bars_.empty()) {
            const double close = bars_.back().close;
            if (is_long  && close < ema9_) return false;
            if (!is_long && close > ema9_) return false;
        }

        return true;
    }

    void _fire_entry(bool is_long, double bid, double ask,
                     int64_t now_ms, double sl_pts, double tp_pts) noexcept {
        const double entry = is_long ? ask : bid;
        if (entry <= 0.0 || atr_pts_ <= 0.0) return;

        pos.active       = true;
        pos.is_long      = is_long;
        pos.entry_px     = entry;
        pos.atr_at_entry = atr_pts_;
        pos.entry_ts_ms  = now_ms;
        pos.mfe          = 0.0;
        pos.mae          = 0.0;
        pos.trail_active = false;
        pos.trail_peak   = entry;

        if (is_long) {
            pos.sl_px = entry - sl_pts;
            pos.tp_px = entry + tp_pts;
        } else {
            pos.sl_px = entry + sl_pts;
            pos.tp_px = entry - tp_pts;
        }
    }

    // ── Position management ─────────────────────────────────────────────
    void _manage_open(double bid, double ask, int64_t now_ms,
                      OnCloseFn on_close) noexcept {
        const double mid = (bid + ask) * 0.5;
        const double fav = pos.is_long ? (mid - pos.entry_px)
                                       : (pos.entry_px - mid);
        if (fav > pos.mfe) pos.mfe = fav;
        if (-fav > pos.mae) pos.mae = -fav;

        // Trail logic: activate when MFE reaches trigger, then trail
        const double trail_trigger = pos.atr_at_entry * trail_trigger_atr;
        const double trail_dist    = pos.atr_at_entry * trail_dist_atr;

        if (!pos.trail_active && pos.mfe >= trail_trigger) {
            pos.trail_active = true;
            pos.trail_peak = mid;
        }

        if (pos.trail_active) {
            // Update peak
            if (pos.is_long && mid > pos.trail_peak) pos.trail_peak = mid;
            if (!pos.is_long && mid < pos.trail_peak) pos.trail_peak = mid;

            // Trail stop
            double trail_sl;
            if (pos.is_long) {
                trail_sl = pos.trail_peak - trail_dist;
                if (trail_sl > pos.sl_px) pos.sl_px = trail_sl;
            } else {
                trail_sl = pos.trail_peak + trail_dist;
                if (trail_sl < pos.sl_px) pos.sl_px = trail_sl;
            }
        }

        // Check SL/TP
        bool hit_sl = false, hit_tp = false;
        double xp = 0.0;
        if (pos.is_long) {
            if (bid <= pos.sl_px) { xp = pos.sl_px; hit_sl = true; }
            else if (bid >= pos.tp_px) { xp = pos.tp_px; hit_tp = true; }
        } else {
            if (ask >= pos.sl_px) { xp = pos.sl_px; hit_sl = true; }
            else if (ask <= pos.tp_px) { xp = pos.tp_px; hit_tp = true; }
        }

        if (!hit_sl && !hit_tp) return;

        const char* reason = hit_tp ? "TP_HIT" :
                             (pos.trail_active ? "TRAIL_HIT" : "SL_HIT");
        _close(xp, reason, now_ms, on_close);
    }

    // ── Close ───────────────────────────────────────────────────────────
    void _close(double exit_px, const char* reason,
                int64_t now_ms, OnCloseFn on_close) noexcept {
        if (!pos.active) return;

        const double pts_move = pos.is_long ? (exit_px - pos.entry_px)
                                            : (pos.entry_px - exit_px);

        TradeRecord tr;
        tr.symbol     = "XAUUSD";
        tr.engine     = "GoldUltimate";
        tr.side       = pos.is_long ? "LONG" : "SHORT";
        tr.entryPrice = pos.entry_px;
        tr.exitPrice  = exit_px;
        tr.tp         = pos.tp_px;
        tr.sl         = pos.sl_px;
        tr.size       = lot;
        tr.entryTs    = pos.entry_ts_ms / 1000;
        tr.exitTs     = now_ms / 1000;
        tr.exitReason = reason;
        tr.regime     = "TREND";
        tr.shadow     = shadow_mode;
        tr.pnl        = pts_move * lot;
        tr.mfe        = pos.mfe;
        tr.mae        = pos.mae;

        if (on_close) on_close(tr);

        pos = {};
    }
};

} // namespace omega
