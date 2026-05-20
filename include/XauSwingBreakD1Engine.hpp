// =============================================================================
//  XauSwingBreakD1Engine.hpp -- Swing break (HH+HL pattern + N-day break)
//
//  PROVENANCE (2026-05-20)
//
//  Mega-sweep finding. New archetype: combines momentum continuation
//  (higher-highs + higher-lows over last 3 bars) with breakout filter
//  (close > N-day prior high). Long-only.
//
//  CONFIG (lookback=10, hold=10, sl_atr=1.5, tp_atr=5.0):
//    Signal:
//      bar[-2].high < bar[-1].high < bar[0].high  AND
//      bar[-2].low  < bar[-1].low  < bar[0].low   AND
//      bar[0].close > max(high of last 10 prior bars)
//    Backtest 2yr XAU daily, cost stress:
//      Cost 1bp:    FUL Sh=5.49
//      Cost 10bp:   FUL Sh=5.08, IS=10.43, OOS=9.45
//      Cost 30bp:   FUL Sh=4.17
//      Cost 50bp:   FUL Sh=3.25
//      n=18 over 670 days, PnL=42.5%, WR=61.1%, mdd=4.3%
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

struct XauSwingBreakD1Params {
    int    lookback_days       = 10;
    int    hold_max_days       = 10;
    double sl_atr_mult         = 1.5;
    double tp_atr_mult         = 5.0;
    double lot                 = 0.01;
    double dollars_per_pt      = 1.0;
    int    atr_period          = 14;
    double max_spread          = 1.0;
    bool   weekend_close_gate  = true;
};

inline XauSwingBreakD1Params make_xau_swing_break_d1_params() { return XauSwingBreakD1Params{}; }

struct XauSwingBreakD1Signal {
    bool        valid   = false;
    double      entry   = 0.0;
    double      sl      = 0.0;
    double      tp      = 0.0;
    double      lot     = 0.0;
    const char* reason  = "";
};

struct XauSwingBreakD1Engine {
    bool   shadow_mode = true;
    bool   enabled     = true;
    XauSwingBreakD1Params p;
    std::string symbol = "XAUUSD";

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;

    struct D1Accum {
        bool active=false; int64_t day_utc=0;
        double open=0, high=0, low=0, close=0;
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
        int days_held=0;
    } pos_;

    int m_trade_id_=0;
    bool has_open_position() const noexcept { return pos_.active; }

    void on_tick(double bid, double ask, int64_t now_ms, CloseCallback on_close) noexcept {
        if (!pos_.active || bid<=0 || ask<=0) return;
        const double mid=(bid+ask)*0.5;
        const double move = mid - pos_.entry;
        if (move>pos_.mfe) pos_.mfe=move;
        if (move<pos_.mae) pos_.mae=move;
        if (bid<=pos_.sl)      _close(bid,"SL_HIT",now_ms,on_close);
        else if (bid>=pos_.tp) _close(bid,"TP_HIT",now_ms,on_close);
    }

    XauSwingBreakD1Signal on_h4_bar(double h4_high, double h4_low, double h4_close,
                                     double bid, double ask, int64_t h4_close_ms,
                                     CloseCallback on_close) noexcept {
        XauSwingBreakD1Signal sig{};
        const int64_t day_utc = h4_close_ms / 86400000LL;

        if (!d1_acc_.active) {
            d1_acc_.active=true; d1_acc_.day_utc=day_utc;
            d1_acc_.open=h4_close; d1_acc_.high=h4_high; d1_acc_.low=h4_low; d1_acc_.close=h4_close;
            return sig;
        }

        if (day_utc != d1_acc_.day_utc) {
            const double bar_close = d1_acc_.close;
            const double bar_high  = d1_acc_.high;
            const double bar_low   = d1_acc_.low;

            const double atr_pre = atr_;
            const int n_prior = (int)d1_highs_.size();
            // Need last 2 prior bars for HH+HL check, plus lookback_days for prior_high
            bool can_check = (n_prior >= std::max(2, p.lookback_days));
            double prior_high = 0.0;
            bool hh_hl = false;
            if (can_check) {
                // higher highs: bar[-2].high (n_prior-2) < bar[-1].high (n_prior-1) < bar_high
                hh_hl = (d1_highs_[n_prior-2] < d1_highs_[n_prior-1])
                     && (d1_highs_[n_prior-1] < bar_high)
                     && (d1_lows_[n_prior-2]  < d1_lows_[n_prior-1])
                     && (d1_lows_[n_prior-1]  < bar_low);
                // prior N-day high (exclusive of current bar)
                int start = n_prior - p.lookback_days;
                prior_high = d1_highs_[start];
                for (int i = start+1; i < n_prior; ++i)
                    if (d1_highs_[i] > prior_high) prior_high = d1_highs_[i];
            }

            d1_highs_.push_back(bar_high);
            d1_lows_ .push_back(bar_low);
            d1_closes_.push_back(bar_close);
            const int keep = std::max(p.lookback_days, p.atr_period) + 3;
            while ((int)d1_highs_.size() > keep) {
                d1_highs_.pop_front();
                d1_lows_ .pop_front();
                d1_closes_.pop_front();
            }

            _update_atr_on_bar_close(bar_high, bar_low, bar_close);
            ++bar_count_;

            if (!pos_.active && enabled
                && can_check && hh_hl
                && atr_pre > 0.0 && prior_high > 0.0
                && bar_close > prior_high
                && (ask - bid) <= p.max_spread)
            {
                const double entry_px = ask;
                const double atr_pct = atr_pre / bar_close;
                const double sl_px = entry_px * (1.0 - p.sl_atr_mult * atr_pct);
                const double tp_px = entry_px * (1.0 + p.tp_atr_mult * atr_pct);

                pos_.active=true;
                pos_.entry=entry_px; pos_.sl=sl_px; pos_.tp=tp_px;
                pos_.lot=p.lot; pos_.mfe=pos_.mae=0;
                pos_.entry_ts_ms=h4_close_ms; pos_.days_held=0;
                ++m_trade_id_;

                printf("[XAU_SWING_BREAK_D1] ENTRY LONG @ %.2f sl=%.2f tp=%.2f lot=%.2f"
                       " prior_high=%.2f atr=%.2f%s\n",
                       entry_px, sl_px, tp_px, p.lot, prior_high, atr_pre,
                       shadow_mode ? " [SHADOW]" : "");
                fflush(stdout);

                sig.valid=true; sig.entry=entry_px; sig.sl=sl_px; sig.tp=tp_px; sig.lot=p.lot;
                sig.reason = "XAU_SWING_BREAK_D1_LONG";
            }

            if (pos_.active) {
                ++pos_.days_held;
                if (pos_.days_held >= p.hold_max_days)
                    _close(bid, "TIMEOUT", h4_close_ms, on_close);
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
        if (mid > pos_.entry) _close(bid, "WEEKEND_CLOSE", now_ms, on_close);
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
        const double pts_move = exit_px - pos_.entry;
        const double pnl_dollars = pts_move * pos_.lot * p.dollars_per_pt;

        printf("[XAU_SWING_BREAK_D1] EXIT reason=%s entry=%.2f exit=%.2f pts=%.2f pnl=%.2f days=%d%s\n",
               reason, pos_.entry, exit_px, pts_move, pnl_dollars, pos_.days_held,
               shadow_mode ? " [SHADOW]" : "");
        fflush(stdout);

        TradeRecord tr{};
        tr.symbol="XAUUSD"; tr.side="LONG";
        tr.entryPrice=pos_.entry; tr.exitPrice=exit_px;
        tr.tp=pos_.tp; tr.sl=pos_.sl; tr.size=pos_.lot;
        tr.pnl=pnl_dollars; tr.mfe=pos_.mfe; tr.mae=pos_.mae;
        tr.entryTs=pos_.entry_ts_ms/1000; tr.exitTs=now_ms/1000;
        tr.exitReason=reason; tr.engine="XauSwingBreakD1";
        if (on_close) on_close(tr);
        pos_ = OpenPos{};
    }
};

} // namespace omega
