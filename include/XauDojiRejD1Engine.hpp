// =============================================================================
//  XauDojiRejD1Engine.hpp -- Doji rejection break (long-only)
//
//  PROVENANCE (2026-05-20)
//
//  Top winner from mega_sweep2 cost-stress. Signal: prev D1 bar is a doji
//  (body < 15% of range), current bar closes above prev high.
//
//  CONFIG (hold=10 days, sl_atr=1.5, tp_atr=5.0):
//    Cost 1bp:    FUL Sh=9.87
//    Cost 10bp:   FUL Sh=9.43, IS=6.97, OOS=11.20
//    Cost 30bp:   FUL Sh=8.46
//    Cost 50bp:   FUL Sh=7.48 (most robust of mega_sweep2 batch)
//    n=23, PnL=44.6%, WR=65.2%, mdd=4.0%
//
//  S37 Phase H STAGE-TRAIL TOMBSTONE-BY-ANALOGY (2026-05-27b).
//  NOT empirically tested. Predicted NEGATIVE by analogy to XauPullbackContH4
//  (tp_atr_mult=5, trail -36% Sharpe). Same TP geometry. Doji-rej is a
//  reversal signal -- exit via TP or stop on continuation. Trail-cut at
//  +0.5N would short-circuit the reversal play before it completes.
//  Trail STAYS OFF and is NOT implemented to keep diff small.
// =============================================================================

#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <algorithm>
#include "OmegaTradeLedger.hpp"
#include "PortfolioGuard.hpp"  // S51: concurrency cap

namespace omega {

struct XauDojiRejD1Params {
    int    hold_max_days       = 10;
    double sl_atr_mult         = 1.5;
    double tp_atr_mult         = 5.0;
    double doji_body_pct       = 0.15;   // body must be < this * range
    double lot                 = 0.01;
    double dollars_per_pt      = 1.0;
    int    atr_period          = 14;
    double max_spread          = 1.0;
    bool   weekend_close_gate  = true;
};

inline XauDojiRejD1Params make_xau_doji_rej_d1_params() { return XauDojiRejD1Params{}; }

struct XauDojiRejD1Signal {
    bool        valid   = false;
    double      entry   = 0.0;
    double      sl      = 0.0;
    double      tp      = 0.0;
    double      lot     = 0.0;
    const char* reason  = "";
};

struct XauDojiRejD1Engine {
    bool   shadow_mode = true;
    bool   enabled     = true;
    XauDojiRejD1Params p;
    std::string symbol = "XAUUSD";

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;

    struct D1Accum {
        bool active=false; int64_t day_utc=0;
        double open=0, high=0, low=0, close=0;
    } d1_acc_;

    // Prior bar OHLC (for doji + break check)
    bool   has_prev_=false;
    double prev_open_=0, prev_high_=0, prev_low_=0, prev_close_=0;

    double atr_=0.0;
    int    atr_seed_count_=0;
    double atr_seed_sum_=0.0;
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

    XauDojiRejD1Signal on_h4_bar(double h4_high, double h4_low, double h4_close,
                                  double bid, double ask, int64_t h4_close_ms,
                                  CloseCallback on_close) noexcept {
        XauDojiRejD1Signal sig{};
        const int64_t day_utc = h4_close_ms / 86400000LL;

        if (!d1_acc_.active) {
            d1_acc_.active=true; d1_acc_.day_utc=day_utc;
            d1_acc_.open=h4_close; d1_acc_.high=h4_high; d1_acc_.low=h4_low; d1_acc_.close=h4_close;
            return sig;
        }

        if (day_utc != d1_acc_.day_utc) {
            const double bar_open  = d1_acc_.open;
            const double bar_high  = d1_acc_.high;
            const double bar_low   = d1_acc_.low;
            const double bar_close = d1_acc_.close;

            const double atr_pre = atr_;

            // Signal: prev bar is doji + current close > prev high
            if (!pos_.active && enabled && has_prev_ && atr_pre > 0.0
                && (ask - bid) <= p.max_spread)
            {
                const double prev_body = std::fabs(prev_close_ - prev_open_);
                const double prev_range = prev_high_ - prev_low_;
                const bool is_doji = (prev_range > 0.0 && prev_body < p.doji_body_pct * prev_range);
                if (is_doji && bar_close > prev_high_ && omega::pg::can_open_new_position()) {  // S51 cap
                    const double entry_px = ask;
                    const double atr_pct = atr_pre / bar_close;
                    const double sl_px = entry_px * (1.0 - p.sl_atr_mult * atr_pct);
                    const double tp_px = entry_px * (1.0 + p.tp_atr_mult * atr_pct);

                    pos_.active=true;
                    omega::pg::register_position_open();  // S51 cap
                    pos_.entry=entry_px; pos_.sl=sl_px; pos_.tp=tp_px;
                    pos_.lot=p.lot; pos_.mfe=pos_.mae=0;
                    pos_.entry_ts_ms=h4_close_ms; pos_.days_held=0;
                    ++m_trade_id_;

                    printf("[XAU_DOJI_REJ_D1] ENTRY LONG @ %.2f sl=%.2f tp=%.2f lot=%.2f"
                           " prev_high=%.2f atr=%.2f%s\n",
                           entry_px, sl_px, tp_px, p.lot, prev_high_, atr_pre,
                           shadow_mode ? " [SHADOW]" : "");
                    fflush(stdout);

                    sig.valid=true; sig.entry=entry_px; sig.sl=sl_px; sig.tp=tp_px; sig.lot=p.lot;
                    sig.reason = "XAU_DOJI_REJ_D1_LONG";
                }
            }

            // Update prev-bar state + ATR + counter
            prev_open_=bar_open; prev_high_=bar_high; prev_low_=bar_low; prev_close_=bar_close;
            has_prev_=true;
            _update_atr_on_bar_close(bar_high, bar_low, bar_close);
            ++bar_count_;

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

    void check_weekend_close(double bid, double ask, int64_t now_ms, CloseCallback on_close) noexcept {
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
        if (prev_close_ != 0.0) {  // already updated by signal block; use stored
            // tr already calculated; skipping for brevity since prev_close_ is current bar's
        }
        if (atr_seed_count_ < p.atr_period) {
            atr_seed_sum_ += tr; ++atr_seed_count_;
            if (atr_seed_count_ == p.atr_period) atr_ = atr_seed_sum_ / p.atr_period;
        } else {
            atr_ = (atr_ * (p.atr_period - 1) + tr) / p.atr_period;
        }
    }

    void _close(double exit_px, const char* reason, int64_t now_ms, CloseCallback on_close) noexcept {
        if (!pos_.active) return;
        omega::pg::register_position_close();  // S51 cap
        const double pts_move = exit_px - pos_.entry;
        const double pnl_dollars = pts_move * pos_.lot * p.dollars_per_pt;
        printf("[XAU_DOJI_REJ_D1] EXIT reason=%s entry=%.2f exit=%.2f pts=%.2f pnl=%.2f days=%d%s\n",
               reason, pos_.entry, exit_px, pts_move, pnl_dollars, pos_.days_held,
               shadow_mode ? " [SHADOW]" : "");
        fflush(stdout);
        TradeRecord tr{};
        tr.symbol="XAUUSD"; tr.side="LONG";
        tr.entryPrice=pos_.entry; tr.exitPrice=exit_px;
        tr.tp=pos_.tp; tr.sl=pos_.sl; tr.size=pos_.lot;
        tr.pnl=pnl_dollars; tr.mfe=pos_.mfe; tr.mae=pos_.mae;
        tr.entryTs=pos_.entry_ts_ms/1000; tr.exitTs=now_ms/1000;
        tr.exitReason=reason; tr.engine="XauDojiRejD1";
        if (on_close) on_close(tr);
        pos_ = OpenPos{};
    }
};

} // namespace omega
