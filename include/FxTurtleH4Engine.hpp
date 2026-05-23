// =============================================================================
//  FxTurtleH4Engine.hpp -- 20-bar Donchian breakout on FX majors, long-only
//
//  PROVENANCE (2026-05-23)
//
//  Built after the post-mortem on the S99 FX kill-switch:
//
//    * The 5 session-open compression-bracket engines
//      (EurusdLondonOpen / GbpusdLondonOpen / UsdjpyAsianOpen /
//      AudusdSydneyOpen / NzdusdAsianOpen) were disabled by S99 because the
//      full BreakoutEngine + BracketEngine sweep across all 5 pairs showed
//      negative expectancy. Pattern: low-trade-frequency micro-compression
//      detection at session opens. Cost-to-target ratio on 8-20 pip TPs
//      with 0.5-1.4 pip broker spread was 5-15%; live edge collapsed.
//
//    * Meanwhile backtest/walkforward_b_long_EURUSD_picks.csv shows the
//      same long-only Donchian H4 pattern that works on GER40 + XAU also
//      works on EURUSD: PF 1.14-1.30 OOS across all 3 walk-forward folds.
//      Same shape that earned Ger40TurtleH4's mega_sweep PF=4.60 OOS.
//
//  This engine is therefore the long-only Donchian H4 cohort applied to FX
//  majors, replacing the session-open bracket cohort with the slow-trend
//  pattern that has empirical OOS edge.
//
//  CONFIG (per-pair via FxTurtleH4Params):
//    lookback=20 H4 bars (~3.3 days), hold=20 H4 bars, sl_atr=1.5, tp_atr=4.0
//    Direction: per-pair `long_only` flag. EUR/GBP/AUD/NZD long-only (USD
//    downtrend secular); USDJPY also long-only (USD-up vs JPY secular).
//
//  Self-contained: builds own H4 OHLC + ATR14 from tick stream (mirrors
//  Ger40TurtleH4Engine pattern). Warm-seeds from an H1 CSV by aggregating
//  4 H1 bars into each H4 bar inline (lets existing EUR/GBP H1 warmup CSVs
//  serve without resampling on disk).
// =============================================================================

#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <fstream>
#include <functional>
#include <string>
#include <algorithm>
#include "OmegaTradeLedger.hpp"
#include "SeedGuard.hpp"

namespace omega {

struct FxTurtleH4Params {
    int    lookback_bars       = 20;
    int    hold_max_h4         = 20;
    double sl_atr_mult         = 1.5;
    double tp_atr_mult         = 4.0;
    double risk_dollars        = 10.0;
    double lot                 = 0.01;
    double pip_size            = 0.0001;  // USD-quoted majors. JPY pair overrides 0.01.
    double dollars_per_pip     = 10.0;    // $10/pip on 1.0 lot for any FX major.
    int    atr_period          = 14;
    double max_spread_pips     = 2.0;     // reject entry if spread > 2 pips (~$20 cost)
    bool   weekend_close_gate  = true;
    bool   long_only           = true;
    // Session filter: block H4 closes outside the venue's deep-liquidity
    // window. EUR/GBP majors trade deepest London-NY overlap (12-20 UTC
    // close hours); blocking 00/04/08 mirrors the Ger40Turtle session
    // fix shipped same day. Per-pair override allowed.
    bool   session_filter      = true;
};

inline FxTurtleH4Params make_eurusd_turtle_h4_params() {
    FxTurtleH4Params p; p.long_only = true;  p.pip_size = 0.0001; return p;
}
inline FxTurtleH4Params make_gbpusd_turtle_h4_params() {
    FxTurtleH4Params p; p.long_only = true;  p.pip_size = 0.0001; return p;
}
inline FxTurtleH4Params make_audusd_turtle_h4_params() {
    FxTurtleH4Params p; p.long_only = true;  p.pip_size = 0.0001; return p;
}
inline FxTurtleH4Params make_nzdusd_turtle_h4_params() {
    FxTurtleH4Params p; p.long_only = true;  p.pip_size = 0.0001; return p;
}
inline FxTurtleH4Params make_usdjpy_turtle_h4_params() {
    FxTurtleH4Params p;
    p.long_only          = true;     // USDJPY trend = USD-up = JPY-weakness secular drift
    p.pip_size           = 0.01;     // JPY-quoted: 1 pip = 0.01 price
    p.dollars_per_pip    = 10.0;     // approximation; varies slightly with USDJPY level
    return p;
}

struct FxTurtleH4Signal {
    bool        valid   = false;
    bool        is_long = false;
    double      entry   = 0.0;
    double      sl      = 0.0;
    double      tp      = 0.0;
    double      lot     = 0.0;
    const char* reason  = "";
};

struct FxTurtleH4Engine {
    bool   shadow_mode = true;
    bool   enabled     = true;
    FxTurtleH4Params p;
    std::string symbol = "FX";
    std::string warmup_csv_path;  // H1 CSV; aggregator turns 4 H1 -> 1 H4 inline.

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;

    struct H4Accum {
        bool     active     = false;
        int64_t  bucket_ms  = 0;
        double   open       = 0.0;
        double   high       = 0.0;
        double   low        = 0.0;
        double   close      = 0.0;
    } h4_acc_;

    std::deque<double> h4_highs_;
    std::deque<double> h4_lows_;
    std::deque<double> h4_closes_;

    double atr_=0.0;
    int    atr_seed_count_=0;
    double atr_seed_sum_=0.0;
    double prev_h4_close_=0.0;
    int    bar_count_=0;

    struct OpenPos {
        bool active=false;
        bool is_long=false;
        double entry=0, sl=0, tp=0, lot=0, mfe=0, mae=0;
        int64_t entry_ts_ms=0;
        int bars_held=0;
    } pos_;

    int m_trade_id_=0;
    bool has_open_position() const noexcept { return pos_.active; }

    // Tick handler. Manages open position every tick; on H4 bucket
    // rollover, evaluates Donchian breakout entry. Long-only by default
    // (per S99 post-mortem: bidirectional Donchian on FX loses on the
    // short leg against secular USD-down / JPY-weak drift).
    FxTurtleH4Signal on_tick(double bid, double ask, int64_t now_ms,
                              CloseCallback on_close) noexcept {
        FxTurtleH4Signal sig{};
        if (bid <= 0.0 || ask <= 0.0) return sig;
        const double mid = (bid + ask) * 0.5;

        // Manage open position on every tick
        if (pos_.active) {
            const double move = pos_.is_long ? (mid - pos_.entry)
                                              : (pos_.entry - mid);
            if (move > pos_.mfe) pos_.mfe = move;
            if (move < pos_.mae) pos_.mae = move;
            if (pos_.is_long) {
                if (bid <= pos_.sl)      { _close(bid, "SL_HIT", now_ms, on_close); return sig; }
                else if (bid >= pos_.tp) { _close(bid, "TP_HIT", now_ms, on_close); return sig; }
            } else {
                if (ask >= pos_.sl)      { _close(ask, "SL_HIT", now_ms, on_close); return sig; }
                else if (ask <= pos_.tp) { _close(ask, "TP_HIT", now_ms, on_close); return sig; }
            }
        }

        const int64_t bucket = (now_ms / 14400000LL) * 14400000LL;
        if (!h4_acc_.active) {
            h4_acc_.active=true; h4_acc_.bucket_ms=bucket;
            h4_acc_.open=mid; h4_acc_.high=mid; h4_acc_.low=mid; h4_acc_.close=mid;
            return sig;
        }
        if (bucket != h4_acc_.bucket_ms) {
            // H4 bar rollover: snapshot the closing bar + evaluate entry.
            const double bar_high = h4_acc_.high;
            const double bar_low  = h4_acc_.low;
            const double bar_close= h4_acc_.close;

            const double atr_pre = atr_;
            const int n_prior = (int)h4_highs_.size();
            double prior_high = 0.0;
            double prior_low  = 1e18;
            if (n_prior >= p.lookback_bars) {
                int start = n_prior - p.lookback_bars;
                prior_high = h4_highs_[start];
                prior_low  = h4_lows_[start];
                for (int i = start+1; i < n_prior; ++i) {
                    if (h4_highs_[i] > prior_high) prior_high = h4_highs_[i];
                    if (h4_lows_[i]  < prior_low)  prior_low  = h4_lows_[i];
                }
            }

            h4_highs_.push_back(bar_high);
            h4_lows_ .push_back(bar_low);
            h4_closes_.push_back(bar_close);
            const int keep = std::max(p.lookback_bars, p.atr_period) + 2;
            while ((int)h4_highs_.size() > keep) {
                h4_highs_.pop_front();
                h4_lows_ .pop_front();
                h4_closes_.pop_front();
            }

            _update_atr_on_bar_close(bar_high, bar_low, bar_close);
            ++bar_count_;

            // Session filter: only allow entries on H4 closes that contain
            // London cash + NY overlap activity. Close hours 12/16/20 UTC
            // satisfy that. Blocks 00/04/08 (Asian + thin Europe pre-open).
            const int close_hour_utc = static_cast<int>((bucket / 3600000LL) % 24);
            const bool session_ok = !p.session_filter
                                  || close_hour_utc == 12
                                  || close_hour_utc == 16
                                  || close_hour_utc == 20;

            const double spread_pips = (ask - bid) / p.pip_size;

            if (!pos_.active && enabled
                && session_ok
                && n_prior >= p.lookback_bars && atr_pre > 0.0
                && spread_pips <= p.max_spread_pips)
            {
                bool fire_long  = (bar_close > prior_high);
                bool fire_short = (!p.long_only) && (bar_close < prior_low);

                if (fire_long || fire_short) {
                    const bool is_long = fire_long;
                    const double entry_px  = is_long ? ask : bid;
                    const double sl_dist   = p.sl_atr_mult * atr_pre;
                    const double tp_dist   = p.tp_atr_mult * atr_pre;
                    const double sl_px     = is_long ? entry_px - sl_dist
                                                     : entry_px + sl_dist;
                    const double tp_px     = is_long ? entry_px + tp_dist
                                                     : entry_px - tp_dist;
                    const double sl_pips   = sl_dist / p.pip_size;
                    double size = (sl_pips > 0.0)
                        ? p.risk_dollars / (sl_pips * p.dollars_per_pip)
                        : 0.0;
                    size = std::floor(size / 0.01) * 0.01;
                    size = std::max(0.01, std::min(p.lot * 10, size));

                    pos_.active=true;
                    pos_.is_long=is_long;
                    pos_.entry=entry_px; pos_.sl=sl_px; pos_.tp=tp_px;
                    pos_.lot=size; pos_.mfe=pos_.mae=0;
                    pos_.entry_ts_ms=now_ms; pos_.bars_held=0;
                    ++m_trade_id_;

                    printf("[FX_TURTLE_H4] ENTRY %s @ %.5f sl=%.5f tp=%.5f lot=%.3f"
                           " prior20=%.5f atr=%.5f spread=%.1fp%s\n",
                           is_long ? "LONG" : "SHORT",
                           entry_px, sl_px, tp_px, size,
                           is_long ? prior_high : prior_low,
                           atr_pre, spread_pips,
                           shadow_mode ? " [SHADOW]" : "");
                    fflush(stdout);

                    sig.valid=true; sig.is_long=is_long;
                    sig.entry=entry_px; sig.sl=sl_px; sig.tp=tp_px; sig.lot=size;
                    sig.reason = is_long ? "FX_TURTLE_H4_LONG" : "FX_TURTLE_H4_SHORT";
                }
            }

            if (pos_.active) {
                ++pos_.bars_held;
                if (pos_.bars_held >= p.hold_max_h4)
                    _close(pos_.is_long ? bid : ask, "TIMEOUT", now_ms, on_close);
            }

            h4_acc_.bucket_ms=bucket;
            h4_acc_.open=mid; h4_acc_.high=mid; h4_acc_.low=mid; h4_acc_.close=mid;
        } else {
            if (mid > h4_acc_.high) h4_acc_.high = mid;
            if (mid < h4_acc_.low)  h4_acc_.low  = mid;
            h4_acc_.close = mid;
        }
        return sig;
    }

    void check_weekend_close(double bid, double ask, int64_t now_ms,
                             CloseCallback on_close) noexcept {
        if (!pos_.active || !p.weekend_close_gate) return;
        const int64_t utc_sec = now_ms / 1000LL;
        const int dow = static_cast<int>((utc_sec / 86400LL + 3) % 7);
        const int hour = static_cast<int>((utc_sec % 86400LL) / 3600LL);
        if (dow != 4 || hour < 20) return;
        const double mid = (bid+ask)*0.5;
        const bool in_profit = pos_.is_long ? (mid > pos_.entry)
                                            : (mid < pos_.entry);
        if (in_profit) _close(pos_.is_long ? bid : ask, "WEEKEND_CLOSE", now_ms, on_close);
    }

    void cancel() noexcept { pos_ = OpenPos{}; }

    // Warm-seed from H1 CSV. Aggregates every 4 H1 bars into 1 H4 bar
    // inline so the existing EUR/GBP H1 warmup CSVs can drive this engine
    // directly without an offline resample step.
    //
    // CSV format (matches phase1/signal_discovery/warmup_EURUSD_H1.csv):
    //   ts,o,h,l,c      (ts in SECONDS)
    //
    // H4 bucket = floor(ts_sec / 14400) * 14400. Bars are accumulated in
    // bucket order and flushed when the bucket rolls.
    int warmup_from_csv(const std::string& path) noexcept {
        const std::string actual = omega::resolve_seed_path(path);
        std::ifstream f(actual);
        if (!f.is_open()) {
            omega::seed_die(symbol.c_str(), actual);  // [[noreturn]]
        }
        std::string line; std::getline(f, line);  // header

        int64_t cur_bucket = -1;
        double cur_h=0, cur_l=0, cur_c=0;  // open not needed for Donchian/ATR
        bool   have_cur = false;
        int    n_h4 = 0;

        auto flush_bucket = [&]() {
            if (!have_cur) return;
            // Append to engine state, same as live H4 close path.
            h4_highs_.push_back(cur_h);
            h4_lows_ .push_back(cur_l);
            h4_closes_.push_back(cur_c);
            const int keep = std::max(p.lookback_bars, p.atr_period) + 2;
            while ((int)h4_highs_.size() > keep) {
                h4_highs_.pop_front();
                h4_lows_ .pop_front();
                h4_closes_.pop_front();
            }
            _update_atr_on_bar_close(cur_h, cur_l, cur_c);
            ++bar_count_;
            ++n_h4;
        };

        while (std::getline(f, line)) {
            long long ts_s_ll=0; double o=0,h=0,l=0,c=0;
            if (sscanf(line.c_str(), "%lld,%lf,%lf,%lf,%lf",
                       &ts_s_ll, &o, &h, &l, &c) != 5) continue;
            const int64_t bucket = (static_cast<int64_t>(ts_s_ll) / 14400LL) * 14400LL;
            if (bucket != cur_bucket) {
                flush_bucket();
                cur_bucket = bucket;
                (void)o;
                cur_h=h; cur_l=l; cur_c=c;
                have_cur = true;
            } else {
                if (h > cur_h) cur_h = h;
                if (l < cur_l) cur_l = l;
                cur_c = c;
            }
        }
        flush_bucket();  // last bucket

        if (n_h4 < p.lookback_bars) {
            printf("[SEED-FATAL] %s: only %d H4 bars from %s (need >= %d)\n",
                   symbol.c_str(), n_h4, actual.c_str(), p.lookback_bars);
            fflush(stdout);
            omega::seed_die(symbol.c_str(), actual);  // [[noreturn]]
        }
        printf("[SEED] %s: %d H4 bars aggregated from H1 -> hot (atr=%.5f bars=%d) [%s]\n",
               symbol.c_str(), n_h4, atr_, bar_count_, actual.c_str());
        fflush(stdout);
        return n_h4;
    }

    void _update_atr_on_bar_close(double bar_h, double bar_l, double bar_c) noexcept {
        double tr = bar_h - bar_l;
        if (prev_h4_close_ > 0.0) {
            tr = std::max(tr, std::fabs(bar_h - prev_h4_close_));
            tr = std::max(tr, std::fabs(bar_l - prev_h4_close_));
        }
        prev_h4_close_ = bar_c;
        if (atr_seed_count_ < p.atr_period) {
            atr_seed_sum_ += tr; ++atr_seed_count_;
            if (atr_seed_count_ == p.atr_period) atr_ = atr_seed_sum_ / p.atr_period;
        } else {
            atr_ = (atr_ * (p.atr_period - 1) + tr) / p.atr_period;
        }
    }

    void _close(double exit_px, const char* reason, int64_t now_ms,
                CloseCallback on_close) noexcept {
        if (!pos_.active) return;
        const double pts_move  = pos_.is_long ? (exit_px - pos_.entry)
                                              : (pos_.entry - exit_px);
        const double pips_move = pts_move / p.pip_size;
        const double pnl_dollars = pips_move * pos_.lot * p.dollars_per_pip;

        printf("[FX_TURTLE_H4] EXIT %s reason=%s entry=%.5f exit=%.5f pips=%.1f pnl=%.2f bars=%d%s\n",
               symbol.c_str(), reason, pos_.entry, exit_px, pips_move, pnl_dollars, pos_.bars_held,
               shadow_mode ? " [SHADOW]" : "");
        fflush(stdout);

        TradeRecord tr{};
        tr.symbol=symbol; tr.side=pos_.is_long ? "LONG" : "SHORT";
        tr.entryPrice=pos_.entry; tr.exitPrice=exit_px;
        tr.tp=pos_.tp; tr.sl=pos_.sl; tr.size=pos_.lot;
        tr.pnl=pnl_dollars; tr.mfe=pos_.mfe; tr.mae=pos_.mae;
        tr.entryTs=pos_.entry_ts_ms/1000; tr.exitTs=now_ms/1000;
        tr.exitReason=reason; tr.engine="FxTurtleH4";
        if (on_close) on_close(tr);
        pos_ = OpenPos{};
    }
};

}  // namespace omega
