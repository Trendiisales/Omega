// =============================================================================
//  NasTurtleD1Engine.hpp -- 20-day Donchian break on NAS100 D1 (long-only)
//
//  PROVENANCE (2026-06-14)
//
//  Seykota/Donchian D1 archetype extended to NAS100. Clone of
//  Ger40TurtleH4Engine (proven index-turtle chassis: self-aggregates bars from
//  the tick stream, long-only, index VIX risk-off gate) retuned to D1 (86400s
//  bucket) + NAS100. Differences vs the GER40 template: D1 bucket, lookback=20
//  days, NAS100 symbol/cost-gate, the DAX-cash session filter removed (NAS D1
//  bars close at 00:00 UTC; no intraday session gate).
//
//  BACKTEST (independent Yahoo daily 2016-2026, robust config ema100/don20):
//    NAS100 long-only MAR 0.44, PF 2.10, Sharpe 0.58. One of only two trend
//    horses in the Omega universe (the other = XAU, [[XauTurtleD1Engine]]); FX
//    and EU indices were DEAD for breakout-trend. Reconfirms the FVG/Peachy NAS
//    edge from a 3rd data source. SHADOW until >=5 live shadow trades.
//
//  NOTE: cold-warm (NO mandatory seed CSV -> no seed_die crash risk). Needs
//  ~20 live D1 bars (~4 weeks) before the first possible signal. Exit uses the
//  GER40 chassis TP/SL (sl 1.5*ATR / tp 5.0*ATR); the Yahoo test favoured a
//  wide-trail no-TP exit -> revisit exit after first shadow trades.
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
#include "OmegaCostGuard.hpp"
#include "OpenPositionRegistry.hpp"
#include "SeedGuard.hpp"
#include "IndexRiskGate.hpp"

namespace omega {

struct NasTurtleD1Params {
    int    lookback_bars       = 20;
    int    hold_max_bars       = 20;
    double sl_atr_mult         = 1.5;
    double tp_atr_mult         = 5.0;
    double risk_dollars        = 10.0;
    double lot                 = 0.01;
    double dollars_per_pt      = 1.0;   // NAS100 CFD ~ $1/pt at 1.0 lot (shadow PnL scale)
    int    atr_period          = 14;
    double max_spread          = 30.0;  // NAS100 wide vs gold/DAX
    bool   weekend_close_gate  = true;
};

inline NasTurtleD1Params make_nas_turtle_d1_params() { return NasTurtleD1Params{}; }

struct NasTurtleD1Signal {
    bool        valid   = false;
    double      entry   = 0.0;
    double      sl      = 0.0;
    double      tp      = 0.0;
    double      lot     = 0.0;
    const char* reason  = "";
};

struct NasTurtleD1Engine {
    bool   shadow_mode = true;
    bool   enabled     = true;
    NasTurtleD1Params p;
    std::string symbol = "NAS100";

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;

    struct D1Accum {
        bool     active     = false;
        int64_t  bucket_ms  = 0;
        double   open       = 0.0;
        double   high       = 0.0;
        double   low        = 0.0;
        double   close      = 0.0;
    } d1_acc_;

    std::deque<double> d1_highs_;
    std::deque<double> d1_lows_;
    std::deque<double> d1_closes_;

    double atr_=0.0;
    int    atr_seed_count_=0;
    double atr_seed_sum_=0.0;
    double prev_d1_close_=0.0;
    int    bar_count_=0;

    struct OpenPos {
        bool active=false;
        double entry=0, sl=0, tp=0, lot=0, mfe=0, mae=0;
        int64_t entry_ts_ms=0;
        int bars_held=0;
    } pos_;

    int m_trade_id_=0;
    bool has_open_position() const noexcept { return pos_.active; }

    bool persist_save(const char* eng, const char* sym, omega::PositionSnapshot& o) const {
        if (!pos_.active) return false;
        o.engine = eng; o.symbol = sym; o.side = "LONG";
        o.size = pos_.lot; o.entry = pos_.entry; o.sl = pos_.sl; o.tp = pos_.tp;
        o.entry_ts = pos_.entry_ts_ms / 1000;
        return true;
    }
    bool persist_restore(const omega::PositionSnapshot& ps) {
        pos_.active = true;
        pos_.entry = ps.entry; pos_.sl = ps.sl; pos_.tp = ps.tp; pos_.lot = ps.size;
        pos_.entry_ts_ms = ps.entry_ts * 1000;
        return true;
    }

    void force_close(double bid, double ask, CloseCallback on_close, const char* reason) noexcept {
        if (!pos_.active) return;
        _close(bid, reason, (int64_t)std::time(nullptr) * 1000, on_close);
    }

    NasTurtleD1Signal on_tick(double bid, double ask, int64_t now_ms,
                              CloseCallback on_close) noexcept {
        NasTurtleD1Signal sig{};
        if (bid <= 0.0 || ask <= 0.0) return sig;
        const double mid = (bid + ask) * 0.5;

        if (pos_.active) {
            const double move = mid - pos_.entry;
            if (move > pos_.mfe) pos_.mfe = move;
            if (move < pos_.mae) pos_.mae = move;
            if (bid <= pos_.sl)      { _close(bid, "SL_HIT", now_ms, on_close); return sig; }
            else if (bid >= pos_.tp) { _close(bid, "TP_HIT", now_ms, on_close); return sig; }
        }

        const int64_t bucket = (now_ms / 86400000LL) * 86400000LL;
        if (!d1_acc_.active) {
            d1_acc_.active=true; d1_acc_.bucket_ms=bucket;
            d1_acc_.open=mid; d1_acc_.high=mid; d1_acc_.low=mid; d1_acc_.close=mid;
            return sig;
        }
        if (bucket != d1_acc_.bucket_ms) {
            const double bar_high = d1_acc_.high;
            const double bar_low  = d1_acc_.low;
            const double bar_close= d1_acc_.close;

            const double atr_pre = atr_;
            const int n_prior = (int)d1_highs_.size();
            double prior_high = 0.0;
            if (n_prior >= p.lookback_bars) {
                int start = n_prior - p.lookback_bars;
                prior_high = d1_highs_[start];
                for (int i = start+1; i < n_prior; ++i)
                    if (d1_highs_[i] > prior_high) prior_high = d1_highs_[i];
            }

            d1_highs_.push_back(bar_high);
            d1_lows_ .push_back(bar_low);
            d1_closes_.push_back(bar_close);
            const int keep = std::max(p.lookback_bars, p.atr_period) + 2;
            while ((int)d1_highs_.size() > keep) {
                d1_highs_.pop_front();
                d1_lows_ .pop_front();
                d1_closes_.pop_front();
            }

            _update_atr_on_bar_close(bar_high, bar_low, bar_close);
            ++bar_count_;

            if (!pos_.active && enabled
                && n_prior >= p.lookback_bars && atr_pre > 0.0
                && bar_close > prior_high
                && (ask - bid) <= p.max_spread
                && !omega::index_risk_off())   // S44 portfolio VIX risk-off: no new entry
            {
                const double entry_px = ask;
                const double atr_pct = atr_pre / bar_close;
                const double sl_px = entry_px * (1.0 - p.sl_atr_mult * atr_pct);
                const double tp_px = entry_px * (1.0 + p.tp_atr_mult * atr_pct);

                const double sl_pts = entry_px * p.sl_atr_mult * atr_pct;
                double size = p.risk_dollars / (sl_pts * p.dollars_per_pt);
                size = std::floor(size / 0.01) * 0.01;
                size = std::max(0.01, std::min(p.lot * 10, size));

                if (ExecutionCostGuard::is_viable("NAS100", ask - bid, tp_px - entry_px, size, 1.5)) {

                pos_.active=true;
                pos_.entry=entry_px; pos_.sl=sl_px; pos_.tp=tp_px;
                pos_.lot=size; pos_.mfe=pos_.mae=0;
                pos_.entry_ts_ms=now_ms; pos_.bars_held=0;
                ++m_trade_id_;

                printf("[NAS_TURTLE_D1] ENTRY LONG @ %.2f sl=%.2f tp=%.2f lot=%.3f"
                       " prior20d_high=%.2f atr=%.2f%s\n",
                       entry_px, sl_px, tp_px, size, prior_high, atr_pre,
                       shadow_mode ? " [SHADOW]" : "");
                fflush(stdout);

                sig.valid=true; sig.entry=entry_px; sig.sl=sl_px; sig.tp=tp_px; sig.lot=size;
                sig.reason = "NAS_TURTLE_D1_LONG";
                }
            }

            if (pos_.active) {
                ++pos_.bars_held;
                if (pos_.bars_held >= p.hold_max_bars)
                    _close(bid, "TIMEOUT", now_ms, on_close);
            }

            d1_acc_.bucket_ms=bucket;
            d1_acc_.open=mid; d1_acc_.high=mid; d1_acc_.low=mid; d1_acc_.close=mid;
        } else {
            if (mid > d1_acc_.high) d1_acc_.high = mid;
            if (mid < d1_acc_.low)  d1_acc_.low  = mid;
            d1_acc_.close = mid;
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
        if (mid > pos_.entry) _close(bid, "WEEKEND_CLOSE", now_ms, on_close);
    }

    void cancel() noexcept { pos_ = OpenPos{}; }

    // Warm-seed historical D1 bars directly into deques + ATR state (bypasses
    // tick aggregation). Called once at startup from engine_init.hpp so the
    // 20-bar Donchian + 14-bar ATR are HOT on first live tick instead of
    // cold-warming ~20 trading days. Mirrors Ger40TurtleH4Engine::seed_from_h4_csv.
    // CSV format: ts_ms,open,high,low,close
    size_t seed_from_d1_csv(const std::string& path) noexcept {
        const std::string actual = omega::resolve_seed_path(path);
        std::ifstream f(actual);
        if (!f.is_open()) {
            omega::seed_die("NasTurtleD1", actual);  // [[noreturn]]
        }
        std::string line; std::getline(f, line); // header
        size_t n = 0;
        while (std::getline(f, line)) {
            long long ts_ms_ll=0; double o=0,h=0,l=0,c=0;
            if (sscanf(line.c_str(), "%lld,%lf,%lf,%lf,%lf",
                       &ts_ms_ll, &o, &h, &l, &c) == 5) {
                d1_highs_.push_back(h);
                d1_lows_.push_back(l);
                d1_closes_.push_back(c);
                const int keep = std::max(p.lookback_bars, p.atr_period) + 2;
                while ((int)d1_highs_.size() > keep) {
                    d1_highs_.pop_front();
                    d1_lows_ .pop_front();
                    d1_closes_.pop_front();
                }
                _update_atr_on_bar_close(h, l, c);
                ++bar_count_;
                ++n;
            }
        }
        if (n < static_cast<size_t>(p.lookback_bars)) {
            printf("[SEED-FATAL] NasTurtleD1: only %zu rows in %s (need >= %d)\n",
                   n, actual.c_str(), p.lookback_bars);
            fflush(stdout);
            omega::seed_die("NasTurtleD1", actual);  // [[noreturn]]
        }
        printf("[SEED] NasTurtleD1: %zu D1 bars -> hot (atr=%.2f bars=%d) [%s]\n",
               n, atr_, bar_count_, actual.c_str());
        fflush(stdout);
        return n;
    }

    void _update_atr_on_bar_close(double bar_h, double bar_l, double bar_c) noexcept {
        double tr = bar_h - bar_l;
        if (prev_d1_close_ > 0.0) {
            tr = std::max(tr, std::fabs(bar_h - prev_d1_close_));
            tr = std::max(tr, std::fabs(bar_l - prev_d1_close_));
        }
        prev_d1_close_ = bar_c;
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
        const double pts_move = exit_px - pos_.entry;
        const double pnl_dollars = pts_move * pos_.lot * p.dollars_per_pt;

        printf("[NAS_TURTLE_D1] EXIT reason=%s entry=%.2f exit=%.2f pts=%.2f pnl=%.2f bars=%d%s\n",
               reason, pos_.entry, exit_px, pts_move, pnl_dollars, pos_.bars_held,
               shadow_mode ? " [SHADOW]" : "");
        fflush(stdout);

        TradeRecord tr{};
        tr.symbol="NAS100"; tr.side="LONG";
        tr.entryPrice=pos_.entry; tr.exitPrice=exit_px;
        tr.tp=pos_.tp; tr.sl=pos_.sl; tr.size=pos_.lot;
        tr.pnl=pnl_dollars; tr.mfe=pos_.mfe; tr.mae=pos_.mae;
        tr.entryTs=pos_.entry_ts_ms/1000; tr.exitTs=now_ms/1000;
        tr.exitReason=reason; tr.engine="NasTurtleD1";
        if (on_close) on_close(tr);
        pos_ = OpenPos{};
    }
};

} // namespace omega
