// =============================================================================
//  XauStopRunD1Engine.hpp -- XAU stop-run reversal on D1 (long-only)
//
//  PROVENANCE (2026-05-20)
//
//  Resurrection of the StopRunReversal signal archetype, retired at S50 X2
//  (Apr 27 2026) along with TSMomGold + OverlapFade. Re-tested on current
//  2yr XAU daily data:
//
//    Config (lookback=5, hold=20, sl_atr=1.5, tp_atr=2.0, long-side only):
//      Signal: bar.low <= prior_5d_low AND bar.close > prior_5d_low
//              (stop-hunt below recent floor, then rejection rally)
//
//      Cost 1bps:    IS Sh=7.76, OOS Sh=6.55, FUL Sh=6.84, n=29 PnL=35.7%
//      Cost 10bps:   IS Sh=7.06, OOS Sh=6.14, FUL Sh=6.34
//      Cost 50bps:   IS Sh=3.94, OOS Sh=4.34, FUL Sh=4.12 (cost-robust)
//
//    Robustness +/-20%: most variants FUL Sh 5-10. Some asymmetric
//    (lookback=15 IS Sh=1.01 OOS Sh=13.17 -- skip extreme params).
//    WR=65.5%, 29 trades over 670 days = ~14/year.
//
//  Distinct from MinimalH4Breakout (different timeframe + signal).
//  Distinct from XauTsmomFastD1Engine (momentum-direction, not reversal).
//
//  CAVEAT: n=29 is moderate. Min 5 live shadow trades before LIVE consider.
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

namespace omega {

struct XauStopRunD1Params {
    int    lookback_days       = 5;
    int    hold_max_days       = 20;
    double sl_atr_mult         = 1.5;
    double tp_atr_mult         = 2.0;
    double risk_dollars        = 10.0;
    double lot                 = 0.01;
    double dollars_per_pt      = 1.0;
    int    atr_period          = 14;
    double max_spread          = 1.0;
    bool   weekend_close_gate  = true;
};

inline XauStopRunD1Params make_xau_stop_run_d1_params() { return XauStopRunD1Params{}; }

struct XauStopRunD1Signal {
    bool        valid   = false;
    bool        is_long = false;
    double      entry   = 0.0;
    double      sl      = 0.0;
    double      tp      = 0.0;
    double      lot     = 0.0;
    const char* reason  = "";
};

struct XauStopRunD1Engine {
    bool   shadow_mode = true;
    bool   enabled     = true;
    XauStopRunD1Params p;
    std::string symbol = "XAUUSD";

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;

    struct D1Accum {
        bool active=false; int64_t day_utc=0;
        double open=0, high=0, low=0, close=0;
    } d1_acc_;

    std::deque<double> d1_lows_;
    std::deque<double> d1_highs_;
    std::deque<double> d1_closes_;

    double atr_=0.0;
    int    atr_seed_count_=0;
    double atr_seed_sum_=0.0;
    double prev_d1_close_=0.0;
    int    bar_count_=0;

    struct OpenPos {
        bool active=false, is_long=false;
        double entry=0, sl=0, tp=0, lot=0, mfe=0, mae=0;
        int64_t entry_ts_ms=0;
        int days_held=0;
    } pos_;

    int m_trade_id_=0;
    bool has_open_position() const noexcept { return pos_.active; }

    void on_tick(double bid, double ask, int64_t now_ms, CloseCallback on_close) noexcept {
        if (!pos_.active || bid<=0 || ask<=0) return;
        const double mid=(bid+ask)*0.5;
        const double move = pos_.is_long ? (mid-pos_.entry) : (pos_.entry-mid);
        if (move>pos_.mfe) pos_.mfe=move;
        if (move<pos_.mae) pos_.mae=move;
        if (pos_.is_long) {
            if (bid<=pos_.sl)      _close(bid,"SL_HIT",now_ms,on_close);
            else if (bid>=pos_.tp) _close(bid,"TP_HIT",now_ms,on_close);
        } else {
            if (ask>=pos_.sl)      _close(ask,"SL_HIT",now_ms,on_close);
            else if (ask<=pos_.tp) _close(ask,"TP_HIT",now_ms,on_close);
        }
    }

    XauStopRunD1Signal on_h4_bar(double h4_high, double h4_low, double h4_close,
                                  double bid, double ask, int64_t h4_close_ms,
                                  CloseCallback on_close) noexcept {
        XauStopRunD1Signal sig{};
        const int64_t day_utc = h4_close_ms / 86400000LL;

        if (!d1_acc_.active) {
            d1_acc_.active=true; d1_acc_.day_utc=day_utc;
            d1_acc_.open=h4_close; d1_acc_.high=h4_high; d1_acc_.low=h4_low; d1_acc_.close=h4_close;
            return sig;
        }

        if (day_utc != d1_acc_.day_utc) {
            const double bar_high = d1_acc_.high;
            const double bar_low  = d1_acc_.low;
            const double bar_close= d1_acc_.close;

            // Prior 5-day low (before this bar)
            const double atr_pre = atr_;
            const int n_prior = (int)d1_lows_.size();
            double prior_low = 1e18;
            if (n_prior >= p.lookback_days) {
                int start = n_prior - p.lookback_days;
                prior_low = d1_lows_[start];
                for (int i = start+1; i < n_prior; ++i)
                    if (d1_lows_[i] < prior_low) prior_low = d1_lows_[i];
            }

            d1_lows_  .push_back(bar_low);
            d1_highs_ .push_back(bar_high);
            d1_closes_.push_back(bar_close);
            const int keep = std::max(p.lookback_days, p.atr_period) + 2;
            while ((int)d1_lows_.size() > keep) {
                d1_lows_.pop_front();
                d1_highs_.pop_front();
                d1_closes_.pop_front();
            }

            _update_atr_on_bar_close(bar_high, bar_low, bar_close);
            ++bar_count_;

            // Signal: this bar's low broke prior 5d low AND closed above it
            //         (stop-hunt + rejection rally)
            if (!pos_.active && enabled && n_prior >= p.lookback_days
                && atr_pre > 0.0 && prior_low < 1e17
                && (ask - bid) <= p.max_spread
                && bar_low < prior_low
                && bar_close > prior_low)
            {
                const double entry_px = ask;
                const double atr_pct = atr_pre / bar_close;
                const double sl_px = entry_px * (1.0 - p.sl_atr_mult * atr_pct);
                const double tp_px = entry_px * (1.0 + p.tp_atr_mult * atr_pct);

                pos_.active=true; pos_.is_long=true;
                pos_.entry=entry_px; pos_.sl=sl_px; pos_.tp=tp_px;
                pos_.lot=p.lot; pos_.mfe=pos_.mae=0;
                pos_.entry_ts_ms=h4_close_ms; pos_.days_held=0;
                ++m_trade_id_;

                printf("[XAU_STOPRUN_D1] ENTRY LONG @ %.2f sl=%.2f tp=%.2f lot=%.2f"
                       " prior5d_low=%.2f bar_low=%.2f close=%.2f%s\n",
                       entry_px, sl_px, tp_px, p.lot, prior_low, bar_low, bar_close,
                       shadow_mode ? " [SHADOW]" : "");
                fflush(stdout);

                sig.valid=true; sig.is_long=true;
                sig.entry=entry_px; sig.sl=sl_px; sig.tp=tp_px; sig.lot=p.lot;
                sig.reason = "XAU_STOPRUN_D1_LONG";
            }

            if (pos_.active) {
                ++pos_.days_held;
                if (pos_.days_held >= p.hold_max_days)
                    _close(pos_.is_long ? bid : ask, "TIMEOUT", h4_close_ms, on_close);
            }

            d1_acc_.day_utc=day_utc;
            d1_acc_.open=h4_close; d1_acc_.high=h4_high; d1_acc_.low=h4_low; d1_acc_.close=h4_close;
        } else {
            if (h4_high > d1_acc_.high) d1_acc_.high = h4_high;
            if (h4_low  < d1_acc_.low)  d1_acc_.low  = h4_low;
            d1_acc_.close = h4_close;
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
        const double move = pos_.is_long ? (mid-pos_.entry) : (pos_.entry-mid);
        if (move > 0.0) _close(pos_.is_long ? bid : ask, "WEEKEND_CLOSE", now_ms, on_close);
    }

    void cancel() noexcept { pos_ = OpenPos{}; }

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
        const double pts_move = pos_.is_long ? (exit_px - pos_.entry) : (pos_.entry - exit_px);
        const double pnl_dollars = pts_move * pos_.lot * p.dollars_per_pt;

        printf("[XAU_STOPRUN_D1] EXIT %s reason=%s entry=%.2f exit=%.2f pts=%.2f"
               " pnl=%.2f days=%d%s\n",
               pos_.is_long ? "LONG" : "SHORT", reason, pos_.entry, exit_px,
               pts_move, pnl_dollars, pos_.days_held,
               shadow_mode ? " [SHADOW]" : "");
        fflush(stdout);

        TradeRecord tr{};
        tr.symbol="XAUUSD"; tr.side=pos_.is_long?"LONG":"SHORT";
        tr.entryPrice=pos_.entry; tr.exitPrice=exit_px;
        tr.tp=pos_.tp; tr.sl=pos_.sl; tr.size=pos_.lot;
        tr.pnl=pnl_dollars; tr.mfe=pos_.mfe; tr.mae=pos_.mae;
        tr.entryTs=pos_.entry_ts_ms/1000; tr.exitTs=now_ms/1000;
        tr.exitReason=reason; tr.engine="XauStopRunD1";
        if (on_close) on_close(tr);
        pos_ = OpenPos{};
    }
};

} // namespace omega
