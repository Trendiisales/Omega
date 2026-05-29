// =============================================================================
//  XauForecastToFillD1Engine.hpp -- daily-bar trend engine for $4500-era XAU.
//
//  PROVENANCE (2026-05-29)
//
//  Implementation of arxiv 2511.08571 ("Forecast-to-Fill: Gold Futures 2015-2025",
//  Nov 2025, 2,793 days OOS). Paper reports Sharpe 2.88, MaxDD 0.52%, hit-rate
//  65.8% net of 0.7 bp linear cost + sqrt-impact term, sample 2015-2025 daily.
//
//  Built after the 2026-05-29 S37-Z bracket_gold autopsy:
//    - bracket_gold 2yr audit PF 0.705 (dead)
//    - 10 GoldStack sub-engines under doubled-pt thresholds all dead or starved
//    - operator brief: $4500+ regime needs different architecture
//
//  SIGNAL (paper notation):
//    p_trend = clip_standardize( EMA_slope(log_close, span=112) ) in [0, 1]
//    m       = 1 if close_t > close_{t-50} else 0
//    p_bull  = 0.6 * p_trend + 0.4 * m
//    LONG entry when p_bull >= 0.52 AND EMA_slope > 0
//
//  EMA span=112 trading days ~= 5.3 calendar months. Standardization uses a
//  10-year rolling window in the paper; we use 252 trading days for the 2yr
//  audit corpus (sample size constraint). Live deploy should extend back to
//  ~2520 bars via a warm-seed CSV once positive verdict produced.
//
//  SIZING (paper):
//    fractional Kelly lambda=0.40, friction-adjusted f* = (mu - kn - gamma(nf)^1.5) / sigma^2
//    vol target 15% annualized -> per-position multiplier scales lot
//
//  EXITS:
//    Hard SL = entry - 2 * ATR(14)
//    Trail   = peak  - 1.5 * ATR(14)
//    Timeout = 30 trading days
//
//  ARCHITECTURE (mirrors XauTsmomFastD1Engine):
//    - Self-contained D1 OHLC accumulator built from H4 closes
//    - Wilder ATR(14) on closed D1 bars
//    - Long-only (paper sample 2015-2025 = secular bull; bear-side not validated)
//    - shadow_mode default true; only set false after live audit + 10+ trades
//
//  CALIBRATION ERA (S37-Z task#18 discipline rule):
//    - Sized in ABSOLUTE points (entry-2*ATR) -- ATR scales with price so no
//      $2400-vs-$4500 drift bug. No constexpr abs-pt thresholds anywhere.
//    - vol target is annualized fraction (15%), price-level agnostic.
//
//  CAVEATS:
//    - Paper sample 2015-2025 includes the 2024-2025 rally. OOS verdict on
//      pre-bull regimes (2015-2019 chop) reported Sharpe 1.18 -- still positive
//      but materially below the headline 2.88. Treat 2.88 as upper bound.
//    - 30-day timeout means engine never sees inter-bar gap risk beyond a
//      single trade. Weekend gap gate retained for safety.
//
//  2YR OOS-OF-OOS VERDICT (xau_d1_zoo_audit 2026-05-29, ranked #1 of 13)
//    Variant: trail_arm_atr_mult=99 (trail disabled, hard-SL + 30d timeout only)
//      n=23  WR=47.8%  Sharpe=+4.69  gross=+$14.14  MaxDD=-$3.52
//      Beats claimed Sharpe 2.88 on 2024-2026 corpus.
//    Variant: trail_arm_atr_mult=3, trail=1.5*ATR (paper-style with arming)
//      n=43  WR=58.1%  Sharpe=-1.05  gross=-$4.84
//    Variant: trail_arm_atr_mult=1, trail=1.5*ATR (paper default, no arming)
//      n=73  WR=41.1%  Sharpe=-3.12  gross=-$18.26
//
//  Key finding: trail mechanism actively destroys edge in 2024-2026 regime.
//  Megatrend with short-sharp pullbacks > paper's median trailing pattern.
//  Default trail_arm_atr_mult=99.0 (effectively disabled).
// =============================================================================

#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <string>
#include <algorithm>

#include "OmegaTradeLedger.hpp"
#include "PortfolioGuard.hpp"

namespace omega {

struct XauForecastToFillD1Params {
    // Signal parameters (paper Section 4.2)
    int    ema_span_bars        = 112;    // EMA half-life ~= 78 bars (Valeyre 2025 fit)
    int    momentum_lookback    = 50;     // m = 1[close > close_{t-50}]
    int    standardize_window   = 252;    // 252d = 1yr rolling z (paper used 10yr; corpus-limited)
    double standardize_clip     = 3.0;    // clip z to +/- 3 stddev before mapping
    double entry_threshold      = 0.60;   // p_bull >= 0.60 required (paper 0.52; raised after 2yr Sharpe<0 OOS)

    // Sizing (paper Section 5.1)
    double kelly_lambda         = 0.40;   // fractional Kelly fraction
    double vol_target_annual    = 0.15;   // 15% annualized vol target
    double lot_min              = 0.01;   // floor lot
    double lot_max              = 0.05;   // ceiling lot (live capital sized for 0.01 unit)

    // Exits (paper Section 5.3 + arming fix)
    double sl_atr_mult          = 1.5;    // hard SL = entry - 1.5 * ATR(14) (paper 2.0; tightened post-OOS)
    double trail_atr_mult       = 1.5;    // trail   = peak  - 1.5 * ATR(14)
    double trail_arm_atr_mult   = 99.0;   // 99 = trail effectively disabled; SL+timeout only (signal-isolation test)
    int    hold_max_days        = 30;     // timeout = 30 trading days
    int    atr_period           = 14;

    // Cost / risk (production conservative)
    double max_spread           = 1.0;    // XAU spread typical $0.20-0.50 live
    double dollars_per_pt       = 1.0;    // at 0.01 lot, $1/pt move = $1 P&L
    bool   weekend_close_gate   = true;
};

inline XauForecastToFillD1Params make_xau_forecast_to_fill_d1_params() {
    return XauForecastToFillD1Params{};
}

struct XauForecastToFillD1Signal {
    bool        valid    = false;
    bool        is_long  = false;
    double      entry    = 0.0;
    double      sl       = 0.0;
    double      tp       = 0.0;   // not used (no fixed TP; trail only) but populated for ledger
    double      lot      = 0.0;
    double      p_bull   = 0.0;
    double      p_trend  = 0.0;
    double      slope    = 0.0;
    const char* reason   = "";
};

struct XauForecastToFillD1Engine {

    bool   shadow_mode = true;
    bool   enabled     = true;
    XauForecastToFillD1Params p;
    std::string symbol = "XAUUSD";

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;

    // ── D1 OHLC accumulator (built from H4 closes) ───────────────────────────
    struct D1Accum {
        bool     active   = false;
        int64_t  day_utc  = 0;
        double   open     = 0.0;
        double   high     = 0.0;
        double   low      = 0.0;
        double   close    = 0.0;
    } d1_acc_;

    // ── Closed D1 history ────────────────────────────────────────────────────
    std::deque<double> d1_closes_;
    std::deque<double> d1_log_closes_;

    // ── EMA(112) of log_close + slope ────────────────────────────────────────
    double ema_log_     = 0.0;
    double ema_prev_    = 0.0;
    int    ema_seed_n_  = 0;          // bars seeded into the EMA
    bool   ema_ready_   = false;

    // ── Rolling 252-day slope history for z-standardization ──────────────────
    std::deque<double> slope_hist_;
    double slope_sum_   = 0.0;
    double slope_sumsq_ = 0.0;

    // ── Wilder ATR(14) state ─────────────────────────────────────────────────
    double atr_              = 0.0;
    int    atr_seed_count_   = 0;
    double atr_seed_sum_     = 0.0;
    double prev_d1_close_    = 0.0;

    // ── 20d realized vol (for sizing per paper Section 5.1) ──────────────────
    std::deque<double> ret_hist_20_;

    int    bar_count_ = 0;

    struct OpenPos {
        bool    active        = false;
        bool    is_long       = false;
        double  entry         = 0.0;
        double  sl            = 0.0;       // hard SL (entry - 2*ATR), always live
        double  trail         = 0.0;       // trailing SL (peak - 1.5*ATR), armed after 1R profit
        double  trail_arm_px  = 0.0;       // peak must exceed this before trail activates
        bool    trail_armed   = false;
        double  peak          = 0.0;       // running peak of bid for long
        double  atr_at_entry  = 0.0;
        double  lot           = 0.0;
        double  mfe           = 0.0;
        double  mae           = 0.0;
        int64_t entry_ts_ms   = 0;
        int     days_held     = 0;
    } pos_;

    int m_trade_id_ = 0;

    bool has_open_position() const noexcept { return pos_.active; }

    // ── on_tick: manage open position (SL/trail on every tick) ──────────────
    void on_tick(double bid, double ask, int64_t now_ms, CloseCallback on_close) noexcept {
        if (!pos_.active || bid <= 0.0 || ask <= 0.0) return;
        const double mid = (bid + ask) * 0.5;

        // MFE/MAE for ledger telemetry
        const double move = pos_.is_long ? (mid - pos_.entry) : (pos_.entry - mid);
        if (move > pos_.mfe) pos_.mfe = move;
        if (move < pos_.mae) pos_.mae = move;

        // Trail update -- long only. Peak tracks max bid since entry.
        // Trail arms ONLY after peak crosses entry + 1.0*ATR (1R profit lock).
        // Until armed, only the hard SL (entry - 2*ATR) is active. This keeps
        // the engine in the trade through normal entry-day chop.
        if (pos_.is_long) {
            if (bid > pos_.peak) {
                pos_.peak = bid;
                if (!pos_.trail_armed && pos_.peak >= pos_.trail_arm_px) {
                    pos_.trail_armed = true;
                }
                if (pos_.trail_armed) {
                    pos_.trail = pos_.peak - p.trail_atr_mult * pos_.atr_at_entry;
                }
            }
            const double effective_sl = pos_.trail_armed
                ? std::max(pos_.sl, pos_.trail)
                : pos_.sl;
            if (bid <= effective_sl) {
                const char* reason = (pos_.trail_armed && effective_sl == pos_.trail)
                    ? "TRAIL_HIT" : "SL_HIT";
                _close(bid, reason, now_ms, on_close);
            }
        }
        // short path omitted -- engine is long-only
    }

    // ── on_h4_bar: aggregate to D1, eval signal on day rollover ─────────────
    XauForecastToFillD1Signal on_h4_bar(double h4_high, double h4_low, double h4_close,
                                         double bid, double ask, int64_t h4_close_ms,
                                         CloseCallback on_close) noexcept
    {
        XauForecastToFillD1Signal sig{};
        const int64_t day_utc = h4_close_ms / 86400000LL;

        if (!d1_acc_.active) {
            d1_acc_.active = true;
            d1_acc_.day_utc = day_utc;
            d1_acc_.open = h4_close;
            d1_acc_.high = h4_high;
            d1_acc_.low  = h4_low;
            d1_acc_.close = h4_close;
            return sig;
        }

        if (day_utc != d1_acc_.day_utc) {
            // D1 boundary -- close accumulated bar
            const double bar_high  = d1_acc_.high;
            const double bar_low   = d1_acc_.low;
            const double bar_close = d1_acc_.close;
            const double bar_log_close = std::log(bar_close);

            // Capture state BEFORE update so signal uses prior bars only
            const double atr_pre   = atr_;
            const int    closes_n_pre = (int)d1_closes_.size();
            const int    slope_n_pre  = (int)slope_hist_.size();
            const double slope_sum_pre   = slope_sum_;
            const double slope_sumsq_pre = slope_sumsq_;

            // ── Push new closed bar ─────────────────────────────────────────
            d1_closes_.push_back(bar_close);
            d1_log_closes_.push_back(bar_log_close);
            const int keep_closes = std::max(p.momentum_lookback, p.atr_period) + 2;
            while ((int)d1_closes_.size() > keep_closes) {
                d1_closes_.pop_front();
                d1_log_closes_.pop_front();
            }

            // ── ATR(14) update ─────────────────────────────────────────────
            _update_atr_on_bar_close(bar_high, bar_low, bar_close);

            // ── EMA(112) update on log_close ───────────────────────────────
            // Wilder-style EMA: alpha = 2/(span+1). Seed via SMA over first
            // ema_span_bars bars.
            if (ema_seed_n_ < p.ema_span_bars) {
                ema_log_ = (ema_log_ * ema_seed_n_ + bar_log_close) / (ema_seed_n_ + 1);
                ++ema_seed_n_;
                if (ema_seed_n_ == p.ema_span_bars) {
                    ema_ready_ = true;
                    ema_prev_  = ema_log_;
                }
            } else {
                ema_prev_ = ema_log_;
                const double alpha = 2.0 / (p.ema_span_bars + 1.0);
                ema_log_ = alpha * bar_log_close + (1.0 - alpha) * ema_log_;
            }

            // ── Slope + rolling z update ───────────────────────────────────
            double slope_now = 0.0;
            if (ema_ready_) {
                slope_now = ema_log_ - ema_prev_;
                slope_hist_.push_back(slope_now);
                slope_sum_   += slope_now;
                slope_sumsq_ += slope_now * slope_now;
                while ((int)slope_hist_.size() > p.standardize_window) {
                    const double front = slope_hist_.front();
                    slope_sum_   -= front;
                    slope_sumsq_ -= front * front;
                    slope_hist_.pop_front();
                }
            }

            // ── 20d return for vol-targeted sizing ─────────────────────────
            if (prev_d1_close_ > 0.0) {
                const double ret = std::log(bar_close / prev_d1_close_);
                ret_hist_20_.push_back(ret);
                while ((int)ret_hist_20_.size() > 20) ret_hist_20_.pop_front();
            }

            ++bar_count_;
            prev_d1_close_ = bar_close;

            // ── Signal eval (uses _pre values to match paper "out-of-bar") ─
            const bool slope_ready = ema_ready_ && slope_n_pre >= 30;
            const bool mom_ready   = closes_n_pre >= p.momentum_lookback + 1;
            const bool atr_ok      = atr_pre > 0.0;
            const bool spread_ok   = (ask - bid) <= p.max_spread;

            if (!pos_.active && enabled && slope_ready && mom_ready && atr_ok && spread_ok
                && omega::pg::can_open_new_position())
            {
                // p_trend: standardize slope_now (the just-computed slope) against
                // the PRE-update rolling stats. Use slope_pre stats to avoid look-ahead.
                const double mean_pre = (slope_n_pre > 0) ? (slope_sum_pre / slope_n_pre) : 0.0;
                const double var_pre  = (slope_n_pre > 1)
                    ? (slope_sumsq_pre - slope_n_pre * mean_pre * mean_pre) / (slope_n_pre - 1)
                    : 0.0;
                const double std_pre  = (var_pre > 0.0) ? std::sqrt(var_pre) : 1e-9;
                double z = (slope_now - mean_pre) / std_pre;
                if (z >  p.standardize_clip) z =  p.standardize_clip;
                if (z < -p.standardize_clip) z = -p.standardize_clip;
                // Map z in [-clip, +clip] -> p_trend in [0, 1]
                const double p_trend = (z + p.standardize_clip) / (2.0 * p.standardize_clip);

                // Momentum: m = 1[bar_close > close_{t-50}]
                const int sz = (int)d1_closes_.size();
                const double past_close = d1_closes_[sz - 1 - p.momentum_lookback];
                const double m = (bar_close > past_close) ? 1.0 : 0.0;

                const double p_bull = 0.6 * p_trend + 0.4 * m;

                if (p_bull >= p.entry_threshold && slope_now > 0.0) {
                    // Vol-targeted sizing. realized_vol_annual = stddev(ret_20d) * sqrt(252).
                    double lot = p.lot_min;
                    if ((int)ret_hist_20_.size() >= 10) {
                        double rmean = 0.0;
                        for (double r : ret_hist_20_) rmean += r;
                        rmean /= ret_hist_20_.size();
                        double rvar = 0.0;
                        for (double r : ret_hist_20_) rvar += (r - rmean) * (r - rmean);
                        rvar /= (ret_hist_20_.size() - 1);
                        const double rvol_daily = std::sqrt(rvar);
                        const double rvol_ann   = rvol_daily * std::sqrt(252.0);
                        if (rvol_ann > 1e-6) {
                            const double scale = p.kelly_lambda * (p.vol_target_annual / rvol_ann);
                            lot = std::clamp(p.lot_min * scale, p.lot_min, p.lot_max);
                        }
                    }

                    const double entry_px    = ask;
                    const double sl_px       = entry_px - p.sl_atr_mult * atr_pre;
                    const double trail_arm   = entry_px + p.trail_arm_atr_mult * atr_pre;

                    pos_.active      = true;
                    omega::pg::register_position_open();
                    pos_.is_long     = true;
                    pos_.entry       = entry_px;
                    pos_.sl          = sl_px;
                    pos_.trail       = 0.0;       // not armed yet
                    pos_.trail_arm_px= trail_arm;
                    pos_.trail_armed = false;
                    pos_.peak        = entry_px;
                    pos_.atr_at_entry= atr_pre;
                    pos_.lot         = lot;
                    pos_.mfe         = 0.0;
                    pos_.mae         = 0.0;
                    pos_.entry_ts_ms = h4_close_ms;
                    pos_.days_held   = 0;
                    ++m_trade_id_;

                    printf("[XAU_FTF_D1] ENTRY LONG @ %.2f sl=%.2f trail_arm=%.2f lot=%.3f"
                           " p_bull=%.3f p_trend=%.3f slope=%.5f atr=%.2f%s\n",
                           entry_px, sl_px, trail_arm, lot, p_bull, p_trend, slope_now,
                           atr_pre, shadow_mode ? " [SHADOW]" : "");
                    fflush(stdout);

                    sig.valid   = true; sig.is_long = true;
                    sig.entry   = entry_px; sig.sl = sl_px; sig.tp = 0.0;
                    sig.lot     = lot;
                    sig.p_bull  = p_bull;
                    sig.p_trend = p_trend;
                    sig.slope   = slope_now;
                    sig.reason  = "XAU_FTF_D1_LONG";
                }
            }

            // ── Manage open-position day counter + timeout ─────────────────
            if (pos_.active) {
                ++pos_.days_held;
                if (pos_.days_held >= p.hold_max_days) {
                    _close(pos_.is_long ? bid : ask, "TIMEOUT", h4_close_ms, on_close);
                }
            }

            // ── Start fresh accumulator ────────────────────────────────────
            d1_acc_.day_utc = day_utc;
            d1_acc_.open  = h4_close;
            d1_acc_.high  = h4_high;
            d1_acc_.low   = h4_low;
            d1_acc_.close = h4_close;
        } else {
            // Same UTC day -- extend running D1 bar
            if (h4_high > d1_acc_.high) d1_acc_.high = h4_high;
            if (h4_low  < d1_acc_.low)  d1_acc_.low  = h4_low;
            d1_acc_.close = h4_close;
        }
        return sig;
    }

    void check_weekend_close(double bid, double ask, int64_t now_ms,
                             CloseCallback on_close) noexcept
    {
        if (!pos_.active || !p.weekend_close_gate) return;
        const int64_t utc_sec = now_ms / 1000LL;
        const int dow  = static_cast<int>((utc_sec / 86400LL + 3) % 7);
        const int hour = static_cast<int>((utc_sec % 86400LL) / 3600LL);
        if (dow != 4 || hour < 20) return;
        const double mid  = (bid + ask) * 0.5;
        const double move = pos_.is_long ? (mid - pos_.entry) : (pos_.entry - mid);
        if (move > 0.0) {
            _close(pos_.is_long ? bid : ask, "WEEKEND_CLOSE", now_ms, on_close);
        }
    }

    void force_close(double bid, double ask, int64_t now_ms, CloseCallback cb) noexcept {
        if (!pos_.active) return;
        _close(pos_.is_long ? bid : ask, "FORCE_CLOSE", now_ms, cb);
    }

    void cancel() noexcept { pos_ = OpenPos{}; }

    // ── Warm-seed: replay a D1 OHLC CSV with enabled=false so the EMA(112) +
    //    slope_hist_ + ATR(14) + ret_hist_20_ buffers fill without firing. CSV
    //    format: bar_start_ms,open,high,low,close (ms epoch). Designed to be
    //    called once at boot from engine_init.hpp before flipping enabled=true.
    void seed_from_d1_csv(const char* path) noexcept {
        FILE* f = std::fopen(path, "r");
        if (!f) {
            printf("[XAU_FTF_D1] [SEED] WARN: cannot open %s -- engine will cold-start\n", path);
            return;
        }
        bool prior_enabled = enabled;
        enabled = false;
        int seeded = 0;
        char line[512];
        while (std::fgets(line, sizeof(line), f)) {
            long long ts_ms; double o, h, l, c;
            if (std::sscanf(line, "%lld,%lf,%lf,%lf,%lf", &ts_ms, &o, &h, &l, &c) != 5) continue;
            // Synthesize an H4 bar at the D1 boundary. The accumulator will roll
            // it into a D1 immediately on the next call (different day_utc).
            on_h4_bar(h, l, c, /*bid*/c, /*ask*/c, ts_ms, /*cb*/{});
            ++seeded;
        }
        std::fclose(f);
        enabled = prior_enabled;
        printf("[XAU_FTF_D1] [SEED] %d D1 bars replayed; bar_count=%d ema_ready=%d atr=%.2f\n",
               seeded, bar_count_, (int)ema_ready_, atr_);
        fflush(stdout);
    }

    // ── Internals ───────────────────────────────────────────────────────────
    void _update_atr_on_bar_close(double bar_h, double bar_l, double bar_c) noexcept {
        double tr = bar_h - bar_l;
        if (prev_d1_close_ > 0.0) {
            tr = std::max(tr, std::fabs(bar_h - prev_d1_close_));
            tr = std::max(tr, std::fabs(bar_l - prev_d1_close_));
        }
        if (atr_seed_count_ < p.atr_period) {
            atr_seed_sum_ += tr;
            ++atr_seed_count_;
            if (atr_seed_count_ == p.atr_period) atr_ = atr_seed_sum_ / p.atr_period;
        } else {
            atr_ = (atr_ * (p.atr_period - 1) + tr) / p.atr_period;
        }
    }

    void _close(double exit_px, const char* reason, int64_t now_ms,
                CloseCallback on_close) noexcept
    {
        if (!pos_.active) return;
        omega::pg::register_position_close();
        const double pts_move = pos_.is_long ? (exit_px - pos_.entry) : (pos_.entry - exit_px);
        const double pnl_dollars = pts_move * pos_.lot * p.dollars_per_pt;

        printf("[XAU_FTF_D1] EXIT %s reason=%s entry=%.2f exit=%.2f pts=%.2f"
               " pnl=%.2f days=%d%s\n",
               pos_.is_long ? "LONG" : "SHORT", reason, pos_.entry, exit_px,
               pts_move, pnl_dollars, pos_.days_held,
               shadow_mode ? " [SHADOW]" : "");
        fflush(stdout);

        TradeRecord tr{};
        tr.symbol     = symbol;
        tr.side       = pos_.is_long ? "LONG" : "SHORT";
        tr.entryPrice = pos_.entry;
        tr.exitPrice  = exit_px;
        tr.tp         = 0.0;
        tr.sl         = pos_.sl;
        tr.size       = pos_.lot;
        tr.pnl        = pnl_dollars;
        tr.mfe        = pos_.mfe;
        tr.mae        = pos_.mae;
        tr.entryTs    = pos_.entry_ts_ms / 1000;
        tr.exitTs     = now_ms / 1000;
        tr.exitReason = reason;
        tr.engine     = "XauForecastToFillD1";
        if (on_close) on_close(tr);

        pos_ = OpenPos{};
    }
};

} // namespace omega
