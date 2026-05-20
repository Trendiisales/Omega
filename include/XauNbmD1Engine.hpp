// =============================================================================
//  XauNbmD1Engine.hpp -- Noise Band Momentum on XAU D1 (long-only)
//
//  PROVENANCE (2026-05-20)
//
//  Resurrection of NBM (NoiseBandMomentum) signal pattern. Main has NBM
//  engines for indices (g_nbm_sp, g_nbm_nq, g_nbm_nas, g_nbm_us30,
//  g_nbm_gold_london, g_nbm_oil_london) ALL with enabled=false. Test of
//  the signal on XAU daily shows strong edge — main may have disabled them
//  prematurely.
//
//  Signal: define noise band around EMA20: [EMA-band*ATR, EMA+band*ATR].
//  Long when close > EMA + band*ATR + momentum*ATR (breakout above band).
//
//  CONFIG (atr_band=2.0, momentum_atr=0.3, hold=10, sl_atr=1.5, tp_atr=3.0):
//    Backtest 2yr XAU daily (cost=10bps):
//      IS Sh=9.60, OOS Sh=7.30, FUL Sh=8.01, n=25, PnL=35.6%, WR=64.0%
//
//  Distinct from XauTsmomFastD1 (momentum threshold without band) and
//  XauTurtleD1 (Donchian break, no momentum filter).
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

struct XauNbmD1Params {
    int    ema_period          = 20;
    double atr_band_mult       = 2.0;
    double momentum_atr_mult   = 0.3;
    int    hold_max_days       = 10;
    double sl_atr_mult         = 1.5;
    double tp_atr_mult         = 3.0;
    double lot                 = 0.01;
    double dollars_per_pt      = 1.0;
    int    atr_period          = 14;
    double max_spread          = 1.0;
    bool   weekend_close_gate  = true;
};

inline XauNbmD1Params make_xau_nbm_d1_params() { return XauNbmD1Params{}; }

struct XauNbmD1Signal {
    bool        valid   = false;
    double      entry   = 0.0;
    double      sl      = 0.0;
    double      tp      = 0.0;
    double      lot     = 0.0;
    const char* reason  = "";
};

struct XauNbmD1Engine {
    bool   shadow_mode = true;
    bool   enabled     = true;
    XauNbmD1Params p;
    std::string symbol = "XAUUSD";

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;

    struct D1Accum {
        bool active=false; int64_t day_utc=0;
        double open=0, high=0, low=0, close=0;
    } d1_acc_;

    double ema_=0.0;
    int    ema_count_=0;

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

    XauNbmD1Signal on_h4_bar(double h4_high, double h4_low, double h4_close,
                              double bid, double ask, int64_t h4_close_ms,
                              CloseCallback on_close) noexcept {
        XauNbmD1Signal sig{};
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

            // Capture state BEFORE this bar updates
            const double ema_pre = (ema_count_ >= p.ema_period) ? ema_ : 0.0;
            const double atr_pre = atr_;

            // Update EMA
            const double a = 2.0/(p.ema_period+1);
            ema_ = (ema_count_==0) ? bar_close : a*bar_close + (1-a)*ema_;
            ++ema_count_;

            _update_atr_on_bar_close(bar_high, bar_low, bar_close);
            ++bar_count_;

            if (!pos_.active && enabled
                && ema_count_ > p.ema_period + 1
                && ema_pre > 0.0 && atr_pre > 0.0
                && (ask - bid) <= p.max_spread)
            {
                const double upper_break = ema_pre + p.atr_band_mult * atr_pre + p.momentum_atr_mult * atr_pre;
                if (bar_close > upper_break) {
                    const double entry_px = ask;
                    const double atr_pct = atr_pre / bar_close;
                    const double sl_px = entry_px * (1.0 - p.sl_atr_mult * atr_pct);
                    const double tp_px = entry_px * (1.0 + p.tp_atr_mult * atr_pct);

                    pos_.active=true;
                    pos_.entry=entry_px; pos_.sl=sl_px; pos_.tp=tp_px;
                    pos_.lot=p.lot; pos_.mfe=pos_.mae=0;
                    pos_.entry_ts_ms=h4_close_ms; pos_.days_held=0;
                    ++m_trade_id_;

                    printf("[XAU_NBM_D1] ENTRY LONG @ %.2f sl=%.2f tp=%.2f lot=%.2f"
                           " ema=%.2f upper_brk=%.2f atr=%.2f%s\n",
                           entry_px, sl_px, tp_px, p.lot, ema_pre, upper_break, atr_pre,
                           shadow_mode ? " [SHADOW]" : "");
                    fflush(stdout);

                    sig.valid=true;
                    sig.entry=entry_px; sig.sl=sl_px; sig.tp=tp_px; sig.lot=p.lot;
                    sig.reason = "XAU_NBM_D1_LONG";
                }
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

        printf("[XAU_NBM_D1] EXIT reason=%s entry=%.2f exit=%.2f pts=%.2f"
               " pnl=%.2f days=%d%s\n",
               reason, pos_.entry, exit_px, pts_move, pnl_dollars, pos_.days_held,
               shadow_mode ? " [SHADOW]" : "");
        fflush(stdout);

        TradeRecord tr{};
        tr.symbol="XAUUSD"; tr.side="LONG";
        tr.entryPrice=pos_.entry; tr.exitPrice=exit_px;
        tr.tp=pos_.tp; tr.sl=pos_.sl; tr.size=pos_.lot;
        tr.pnl=pnl_dollars; tr.mfe=pos_.mfe; tr.mae=pos_.mae;
        tr.entryTs=pos_.entry_ts_ms/1000; tr.exitTs=now_ms/1000;
        tr.exitReason=reason; tr.engine="XauNbmD1";
        if (on_close) on_close(tr);
        pos_ = OpenPos{};
    }
};

} // namespace omega
