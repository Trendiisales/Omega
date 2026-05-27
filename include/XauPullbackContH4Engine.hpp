// =============================================================================
//  XauPullbackContH4Engine.hpp -- pullback continuation on XAU H4
//
//  PROVENANCE (2026-05-20)
//
//  Resurrection of PullbackCont signal archetype, retired S49 X5 (Apr 26 2026).
//  Signal: EMA_fast > EMA_slow (uptrend) AND price pulls back to EMA_fast
//  within pullback_atr*ATR, then closes back above EMA_fast.
//
//  CONFIG (ema_fast=10, ema_slow=50, pullback_atr=0.5, hold=40, sl_atr=2.0, tp_atr=5.0):
//    Backtest 2yr XAU H4 (cost=10bps):
//      IS Sh=3.97, OOS Sh=4.06, FUL Sh=3.96, n=97, PnL=53.5%, WR=50.5%
//      Trade density: ~50/year on H4 = ~4/month.
//
//  HIGHER N than D1 variant (97 vs 33). Reasonable balance of signal +
//  statistical confidence. Long-only (XAU uptrend regime 2024-2026).
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
#include "PortfolioGuard.hpp"  // S51: concurrency cap

namespace omega {

struct XauPullbackContH4Params {
    int    ema_fast            = 10;
    int    ema_slow            = 50;
    double pullback_atr        = 0.5;
    int    hold_max_h4         = 40;
    double sl_atr_mult         = 2.0;
    double tp_atr_mult         = 5.0;
    double lot                 = 0.01;
    double dollars_per_pt      = 1.0;
    int    atr_period          = 14;
    double max_spread          = 1.0;
    bool   weekend_close_gate  = true;
};

inline XauPullbackContH4Params make_xau_pullback_cont_h4_params() { return XauPullbackContH4Params{}; }

struct XauPullbackContH4Signal {
    bool        valid   = false;
    double      entry   = 0.0;
    double      sl      = 0.0;
    double      tp      = 0.0;
    double      lot     = 0.0;
    const char* reason  = "";
};

struct XauPullbackContH4Engine {
    bool   shadow_mode = true;
    bool   enabled     = true;
    XauPullbackContH4Params p;
    std::string symbol = "XAUUSD";

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;

    // EMA state (incrementally updated on H4 close)
    double ema_fast_=0, ema_slow_=0;
    int    ema_count_=0;

    // Wilder ATR state
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

    void on_tick(double bid, double ask, int64_t now_ms, CloseCallback on_close) noexcept {
        if (!pos_.active || bid<=0 || ask<=0) return;
        const double mid=(bid+ask)*0.5;
        const double move = mid - pos_.entry;
        if (move>pos_.mfe) pos_.mfe=move;
        if (move<pos_.mae) pos_.mae=move;
        if (bid<=pos_.sl)      _close(bid,"SL_HIT",now_ms,on_close);
        else if (bid>=pos_.tp) _close(bid,"TP_HIT",now_ms,on_close);
    }

    XauPullbackContH4Signal on_h4_bar(double h4_high, double h4_low, double h4_close,
                                       double bid, double ask, int64_t h4_close_ms,
                                       CloseCallback on_close) noexcept {
        XauPullbackContH4Signal sig{};

        // Capture state before this bar
        const double ema_fast_pre = (ema_count_ >= p.ema_fast) ? ema_fast_ : 0.0;
        const double ema_slow_pre = (ema_count_ >= p.ema_slow) ? ema_slow_ : 0.0;
        const double atr_pre = atr_;

        // Update EMAs with this bar's close
        const double af = 2.0/(p.ema_fast+1);
        const double as = 2.0/(p.ema_slow+1);
        if (ema_count_==0) { ema_fast_ = ema_slow_ = h4_close; }
        else {
            ema_fast_ = af*h4_close + (1-af)*ema_fast_;
            ema_slow_ = as*h4_close + (1-as)*ema_slow_;
        }
        ++ema_count_;

        _update_atr_on_bar_close(h4_high, h4_low, h4_close);
        ++bar_count_;

        if (!pos_.active && enabled
            && ema_count_ > p.ema_slow + 1
            && atr_pre > 0.0
            && ema_fast_pre > 0.0 && ema_slow_pre > 0.0
            && ema_fast_pre > ema_slow_pre  // uptrend
            && h4_low <= ema_fast_pre + p.pullback_atr * atr_pre  // pulled back to fast EMA
            && h4_close > ema_fast_pre  // closed back above
            && (ask - bid) <= p.max_spread
            && omega::pg::can_open_new_position())  // S51 cap
        {
            const double entry_px = ask;
            const double atr_pct = atr_pre / h4_close;
            const double sl_px = entry_px * (1.0 - p.sl_atr_mult * atr_pct);
            const double tp_px = entry_px * (1.0 + p.tp_atr_mult * atr_pct);

            pos_.active=true;
            omega::pg::register_position_open();  // S51 cap
            pos_.entry=entry_px; pos_.sl=sl_px; pos_.tp=tp_px;
            pos_.lot=p.lot; pos_.mfe=pos_.mae=0;
            pos_.entry_ts_ms=h4_close_ms; pos_.bars_held=0;
            ++m_trade_id_;

            printf("[XAU_PULLBACK_CONT_H4] ENTRY LONG @ %.2f sl=%.2f tp=%.2f lot=%.2f"
                   " ema_f=%.2f ema_s=%.2f atr=%.2f%s\n",
                   entry_px, sl_px, tp_px, p.lot, ema_fast_pre, ema_slow_pre, atr_pre,
                   shadow_mode ? " [SHADOW]" : "");
            fflush(stdout);

            sig.valid=true;
            sig.entry=entry_px; sig.sl=sl_px; sig.tp=tp_px; sig.lot=p.lot;
            sig.reason = "XAU_PULLBACK_CONT_H4_LONG";
        }

        if (pos_.active) {
            ++pos_.bars_held;
            if (pos_.bars_held >= p.hold_max_h4)
                _close(bid, "TIMEOUT", h4_close_ms, on_close);
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
        omega::pg::register_position_close();  // S51 cap
        const double pts_move = exit_px - pos_.entry;
        const double pnl_dollars = pts_move * pos_.lot * p.dollars_per_pt;

        printf("[XAU_PULLBACK_CONT_H4] EXIT reason=%s entry=%.2f exit=%.2f pts=%.2f"
               " pnl=%.2f bars=%d%s\n",
               reason, pos_.entry, exit_px, pts_move, pnl_dollars, pos_.bars_held,
               shadow_mode ? " [SHADOW]" : "");
        fflush(stdout);

        TradeRecord tr{};
        tr.symbol="XAUUSD"; tr.side="LONG";
        tr.entryPrice=pos_.entry; tr.exitPrice=exit_px;
        tr.tp=pos_.tp; tr.sl=pos_.sl; tr.size=pos_.lot;
        tr.pnl=pnl_dollars; tr.mfe=pos_.mfe; tr.mae=pos_.mae;
        tr.entryTs=pos_.entry_ts_ms/1000; tr.exitTs=now_ms/1000;
        tr.exitReason=reason; tr.engine="XauPullbackContH4";
        if (on_close) on_close(tr);
        pos_ = OpenPos{};
    }
};

} // namespace omega
