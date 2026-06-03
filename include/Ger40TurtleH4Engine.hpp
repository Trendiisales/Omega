// =============================================================================
//  Ger40TurtleH4Engine.hpp -- 20-bar Donchian break on GER40 H4 (long-only)
//
//  PROVENANCE (2026-05-20)
//
//  Mega-sweep finding. Turtle/Donchian archetype works on GER40 H4 as well as
//  XAU D1.
//
//  CONFIG (lookback=20, hold=20, sl_atr=1.5, tp_atr=5.0, long_only=true):
//    Backtest 2yr GER40 H4 (cost 10bps):
//      IS Sh=5.06, OOS Sh=4.60, FUL Sh=5.00
//      n=33, PnL=9.4%, WR=63.6%, mdd=2.0%
//
//  Self-contained: builds own H4 OHLC + ATR14 from tick stream (mirrors
//  MinimalH4GER40Breakout pattern).
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
#include "OpenPositionRegistry.hpp"   // S-2026-06-03: omega::PositionSnapshot for persist
#include "SeedGuard.hpp"
#include "IndexRiskGate.hpp"      // S44 portfolio VIX risk-off gate (entry-only)

namespace omega {

struct Ger40TurtleH4Params {
    int    lookback_bars       = 20;
    int    hold_max_h4         = 20;
    double sl_atr_mult         = 1.5;
    double tp_atr_mult         = 5.0;
    double risk_dollars        = 10.0;
    double lot                 = 0.01;
    double dollars_per_pt      = 25.0;  // GER40/DAX $25/pt at 1.0 lot
    int    atr_period          = 14;
    double max_spread          = 3.0;
    bool   weekend_close_gate  = true;
};

inline Ger40TurtleH4Params make_ger40_turtle_h4_params() { return Ger40TurtleH4Params{}; }

struct Ger40TurtleH4Signal {
    bool        valid   = false;
    double      entry   = 0.0;
    double      sl      = 0.0;
    double      tp      = 0.0;
    double      lot     = 0.0;
    const char* reason  = "";
};

struct Ger40TurtleH4Engine {
    bool   shadow_mode = true;
    bool   enabled     = true;
    Ger40TurtleH4Params p;
    std::string symbol = "GER40";

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
        double entry=0, sl=0, tp=0, lot=0, mfe=0, mae=0;
        int64_t entry_ts_ms=0;
        int bars_held=0;
    } pos_;

    int m_trade_id_=0;
    bool has_open_position() const noexcept { return pos_.active; }

    // S-2026-06-03: open-position persistence across restart. Long-only engine.
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

    Ger40TurtleH4Signal on_tick(double bid, double ask, int64_t now_ms,
                                  CloseCallback on_close) noexcept {
        Ger40TurtleH4Signal sig{};
        if (bid <= 0.0 || ask <= 0.0) return sig;
        const double mid = (bid + ask) * 0.5;

        // Manage open position on every tick
        if (pos_.active) {
            const double move = mid - pos_.entry;
            if (move > pos_.mfe) pos_.mfe = move;
            if (move < pos_.mae) pos_.mae = move;
            if (bid <= pos_.sl)      { _close(bid, "SL_HIT", now_ms, on_close); return sig; }
            else if (bid >= pos_.tp) { _close(bid, "TP_HIT", now_ms, on_close); return sig; }
        }

        const int64_t bucket = (now_ms / 14400000LL) * 14400000LL;
        if (!h4_acc_.active) {
            h4_acc_.active=true; h4_acc_.bucket_ms=bucket;
            h4_acc_.open=mid; h4_acc_.high=mid; h4_acc_.low=mid; h4_acc_.close=mid;
            return sig;
        }
        if (bucket != h4_acc_.bucket_ms) {
            const double bar_high = h4_acc_.high;
            const double bar_low  = h4_acc_.low;
            const double bar_close= h4_acc_.close;

            const double atr_pre = atr_;
            const int n_prior = (int)h4_highs_.size();
            double prior_high = 0.0;
            if (n_prior >= p.lookback_bars) {
                int start = n_prior - p.lookback_bars;
                prior_high = h4_highs_[start];
                for (int i = start+1; i < n_prior; ++i)
                    if (h4_highs_[i] > prior_high) prior_high = h4_highs_[i];
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

            // Session filter (added 2026-05-23 after 08:00 UTC stopout):
            // GER40 H4 bars closing at 00/04/08 UTC contain mostly thin
            // overnight pricing or the Frankfurt-open spike (only ~1h of
            // real DAX cash inside the 04:00-08:00 bar). Breakouts here
            // are predominantly false. Allow only bars whose 4h window
            // sits fully inside Frankfurt cash (closes at 12/16/20 UTC).
            const int close_hour_utc = static_cast<int>((bucket / 3600000LL) % 24);
            const bool session_ok = (close_hour_utc == 12
                                  || close_hour_utc == 16
                                  || close_hour_utc == 20);

            if (!pos_.active && enabled
                && session_ok
                && n_prior >= p.lookback_bars && atr_pre > 0.0
                && bar_close > prior_high
                && (ask - bid) <= p.max_spread
                && !omega::index_risk_off())   // S44 portfolio VIX risk-off: no new entry
            {
                const double entry_px = ask;
                const double atr_pct = atr_pre / bar_close;
                const double sl_px = entry_px * (1.0 - p.sl_atr_mult * atr_pct);
                const double tp_px = entry_px * (1.0 + p.tp_atr_mult * atr_pct);

                // Sizing: $risk_dollars / (sl_pts * dollars_per_pt) -> lots
                const double sl_pts = entry_px * p.sl_atr_mult * atr_pct;
                double size = p.risk_dollars / (sl_pts * p.dollars_per_pt);
                size = std::floor(size / 0.01) * 0.01;
                size = std::max(0.01, std::min(p.lot * 10, size));  // cap at 0.10

                pos_.active=true;
                pos_.entry=entry_px; pos_.sl=sl_px; pos_.tp=tp_px;
                pos_.lot=size; pos_.mfe=pos_.mae=0;
                pos_.entry_ts_ms=now_ms; pos_.bars_held=0;
                ++m_trade_id_;

                printf("[GER40_TURTLE_H4] ENTRY LONG @ %.2f sl=%.2f tp=%.2f lot=%.3f"
                       " prior20h4_high=%.2f atr=%.2f%s\n",
                       entry_px, sl_px, tp_px, size, prior_high, atr_pre,
                       shadow_mode ? " [SHADOW]" : "");
                fflush(stdout);

                sig.valid=true; sig.entry=entry_px; sig.sl=sl_px; sig.tp=tp_px; sig.lot=size;
                sig.reason = "GER40_TURTLE_H4_LONG";
            }

            if (pos_.active) {
                ++pos_.bars_held;
                if (pos_.bars_held >= p.hold_max_h4)
                    _close(bid, "TIMEOUT", now_ms, on_close);
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
        if (mid > pos_.entry) _close(bid, "WEEKEND_CLOSE", now_ms, on_close);
    }

    void cancel() noexcept { pos_ = OpenPos{}; }

    // Warm-seed historical H4 bars directly into deques + ATR state.
    // Bypasses tick aggregation. Called once at startup from engine_init.hpp
    // so engine is HOT on first live tick instead of cold-warming 80h+.
    size_t seed_from_h4_csv(const std::string& path) noexcept {
        const std::string actual = omega::resolve_seed_path(path);
        std::ifstream f(actual);
        if (!f.is_open()) {
            omega::seed_die("Ger40TurtleH4", actual);  // [[noreturn]]
        }
        std::string line; std::getline(f, line); // header
        size_t n = 0;
        while (std::getline(f, line)) {
            long long ts_ms_ll=0; double o=0,h=0,l=0,c=0;
            if (sscanf(line.c_str(), "%lld,%lf,%lf,%lf,%lf",
                       &ts_ms_ll, &o, &h, &l, &c) == 5) {
                h4_highs_.push_back(h);
                h4_lows_.push_back(l);
                h4_closes_.push_back(c);
                const int keep = std::max(p.lookback_bars, p.atr_period) + 2;
                while ((int)h4_highs_.size() > keep) {
                    h4_highs_.pop_front();
                    h4_lows_ .pop_front();
                    h4_closes_.pop_front();
                }
                _update_atr_on_bar_close(h, l, c);
                ++bar_count_;
                ++n;
            }
        }
        if (n < static_cast<size_t>(p.lookback_bars)) {
            // Donchian lookback gate (`n_prior >= lookback_bars`) will never
            // satisfy with fewer rows than that -- engine is effectively cold
            // even though the file opened. Treat as hard fail.
            printf("[SEED-FATAL] Ger40TurtleH4: only %zu rows in %s (need >= %d)\n",
                   n, actual.c_str(), p.lookback_bars);
            fflush(stdout);
            omega::seed_die("Ger40TurtleH4", actual);  // [[noreturn]]
        }
        printf("[SEED] Ger40TurtleH4: %zu H4 bars -> hot (atr=%.2f bars=%d) [%s]\n",
               n, atr_, bar_count_, actual.c_str());
        fflush(stdout);
        return n;
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
        const double pts_move = exit_px - pos_.entry;
        const double pnl_dollars = pts_move * pos_.lot * p.dollars_per_pt;

        printf("[GER40_TURTLE_H4] EXIT reason=%s entry=%.2f exit=%.2f pts=%.2f pnl=%.2f bars=%d%s\n",
               reason, pos_.entry, exit_px, pts_move, pnl_dollars, pos_.bars_held,
               shadow_mode ? " [SHADOW]" : "");
        fflush(stdout);

        TradeRecord tr{};
        tr.symbol="GER40"; tr.side="LONG";
        tr.entryPrice=pos_.entry; tr.exitPrice=exit_px;
        tr.tp=pos_.tp; tr.sl=pos_.sl; tr.size=pos_.lot;
        tr.pnl=pnl_dollars; tr.mfe=pos_.mfe; tr.mae=pos_.mae;
        tr.entryTs=pos_.entry_ts_ms/1000; tr.exitTs=now_ms/1000;
        tr.exitReason=reason; tr.engine="Ger40TurtleH4";
        if (on_close) on_close(tr);
        pos_ = OpenPos{};
    }
};

} // namespace omega
