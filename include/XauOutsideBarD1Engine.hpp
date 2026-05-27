// =============================================================================
//  XauOutsideBarD1Engine.hpp -- Outside bar bullish close (long-only)
//
//  PROVENANCE (2026-05-20)
//
//  Mega_sweep2 cost-stress winner. Signal: current D1 bar engulfs prev bar's
//  range (high > prev_high AND low < prev_low) AND closes above open.
//
//  CONFIG (hold=10 days, sl_atr=1.5, tp_atr=3.0):
//    Cost 1bp:    FUL Sh=6.47
//    Cost 10bp:   FUL Sh=5.88, IS=3.72, OOS=8.39 (OOS > IS!)
//    Cost 30bp:   FUL Sh=4.58
//    Cost 50bp:   FUL Sh=3.27
//    n=34-37, PnL=43.0%, WR=64.7%, mdd=5.0%
//
//  S37 Phase H STAGE-TRAIL TOMBSTONE-BY-ANALOGY (2026-05-27b).
//  NOT empirically tested. Predicted NEGATIVE by analogy to XauPullbackContH4
//  (-36% Sharpe) and XauStopRunD1 (extrapolated -70%+ at tp=2N). This engine
//  has tp_atr_mult=3.0 -- sits between the two priors, so predicted impact
//  is -40 to -60%. Trail STAYS OFF and is NOT implemented to keep diff small.
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

struct XauOutsideBarD1Params {
    int    hold_max_days       = 10;
    double sl_atr_mult         = 1.5;
    double tp_atr_mult         = 3.0;
    double lot                 = 0.01;
    double dollars_per_pt      = 1.0;
    int    atr_period          = 14;
    double max_spread          = 1.0;
    bool   weekend_close_gate  = true;
};

inline XauOutsideBarD1Params make_xau_outside_bar_d1_params() { return XauOutsideBarD1Params{}; }

struct XauOutsideBarD1Signal {
    bool valid=false; double entry=0,sl=0,tp=0,lot=0; const char* reason="";
};

struct XauOutsideBarD1Engine {
    bool shadow_mode=true, enabled=true;
    XauOutsideBarD1Params p;
    std::string symbol="XAUUSD";
    using CloseCallback = std::function<void(const omega::TradeRecord&)>;

    struct D1Accum { bool active=false; int64_t day_utc=0; double open=0,high=0,low=0,close=0; } d1_acc_;
    bool   has_prev_=false;
    double prev_high_=0, prev_low_=0;

    double atr_=0; int atr_seed_count_=0; double atr_seed_sum_=0; double prev_d1_close_=0;
    int bar_count_=0;

    struct OpenPos {
        bool active=false; double entry=0,sl=0,tp=0,lot=0,mfe=0,mae=0;
        int64_t entry_ts_ms=0; int days_held=0;
    } pos_;
    int m_trade_id_=0;
    bool has_open_position() const noexcept { return pos_.active; }

    void on_tick(double bid, double ask, int64_t now_ms, CloseCallback cb) noexcept {
        if (!pos_.active || bid<=0 || ask<=0) return;
        const double mid=(bid+ask)*0.5;
        const double move=mid-pos_.entry;
        if (move>pos_.mfe) pos_.mfe=move;
        if (move<pos_.mae) pos_.mae=move;
        if (bid<=pos_.sl) _close(bid,"SL_HIT",now_ms,cb);
        else if (bid>=pos_.tp) _close(bid,"TP_HIT",now_ms,cb);
    }

    XauOutsideBarD1Signal on_h4_bar(double h4_high, double h4_low, double h4_close,
                                     double bid, double ask, int64_t h4_close_ms,
                                     CloseCallback cb) noexcept {
        XauOutsideBarD1Signal sig{};
        const int64_t day_utc = h4_close_ms / 86400000LL;
        if (!d1_acc_.active) {
            d1_acc_.active=true; d1_acc_.day_utc=day_utc;
            d1_acc_.open=h4_close; d1_acc_.high=h4_high; d1_acc_.low=h4_low; d1_acc_.close=h4_close;
            return sig;
        }
        if (day_utc != d1_acc_.day_utc) {
            const double bo=d1_acc_.open, bh=d1_acc_.high, bl=d1_acc_.low, bc=d1_acc_.close;
            const double atr_pre = atr_;

            if (!pos_.active && enabled && has_prev_ && atr_pre>0
                && (ask-bid)<=p.max_spread)
            {
                // Outside bar: bh > prev_high AND bl < prev_low
                const bool outside = (bh > prev_high_) && (bl < prev_low_);
                const bool bullish = bc > bo;
                if (outside && bullish && omega::pg::can_open_new_position()) {  // S51 cap
                    const double entry_px = ask;
                    const double atr_pct = atr_pre / bc;
                    const double sl_px = entry_px * (1.0 - p.sl_atr_mult*atr_pct);
                    const double tp_px = entry_px * (1.0 + p.tp_atr_mult*atr_pct);
                    pos_.active=true; pos_.entry=entry_px; pos_.sl=sl_px; pos_.tp=tp_px;
                    omega::pg::register_position_open();  // S51 cap
                    pos_.lot=p.lot; pos_.mfe=pos_.mae=0;
                    pos_.entry_ts_ms=h4_close_ms; pos_.days_held=0; ++m_trade_id_;
                    printf("[XAU_OUTSIDE_BAR_D1] ENTRY LONG @ %.2f sl=%.2f tp=%.2f lot=%.2f"
                           " prev_h=%.2f prev_l=%.2f atr=%.2f%s\n",
                           entry_px, sl_px, tp_px, p.lot, prev_high_, prev_low_, atr_pre,
                           shadow_mode ? " [SHADOW]" : "");
                    fflush(stdout);
                    sig.valid=true; sig.entry=entry_px; sig.sl=sl_px; sig.tp=tp_px; sig.lot=p.lot;
                    sig.reason="XAU_OUTSIDE_BAR_D1_LONG";
                }
            }

            prev_high_=bh; prev_low_=bl; has_prev_=true;
            _update_atr(bh, bl, bc);
            ++bar_count_;

            if (pos_.active) {
                ++pos_.days_held;
                if (pos_.days_held >= p.hold_max_days)
                    _close(bid, "TIMEOUT", h4_close_ms, cb);
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

    void check_weekend_close(double bid, double ask, int64_t now_ms, CloseCallback cb) noexcept {
        if (!pos_.active || !p.weekend_close_gate) return;
        const int64_t utc_sec = now_ms / 1000LL;
        const int dow = static_cast<int>((utc_sec / 86400LL + 3) % 7);
        const int hour = static_cast<int>((utc_sec % 86400LL) / 3600LL);
        if (dow != 4 || hour < 20) return;
        const double mid = (bid+ask)*0.5;
        if (mid > pos_.entry) _close(bid, "WEEKEND_CLOSE", now_ms, cb);
    }
    void cancel() noexcept { pos_ = OpenPos{}; }

    void _update_atr(double bh, double bl, double bc) noexcept {
        double tr = bh-bl;
        if (prev_d1_close_>0) {
            tr=std::max(tr, std::fabs(bh-prev_d1_close_));
            tr=std::max(tr, std::fabs(bl-prev_d1_close_));
        }
        prev_d1_close_=bc;
        if (atr_seed_count_<p.atr_period) {
            atr_seed_sum_+=tr; ++atr_seed_count_;
            if (atr_seed_count_==p.atr_period) atr_=atr_seed_sum_/p.atr_period;
        } else atr_=(atr_*(p.atr_period-1)+tr)/p.atr_period;
    }

    void _close(double exit_px, const char* reason, int64_t now_ms, CloseCallback cb) noexcept {
        if (!pos_.active) return;
        omega::pg::register_position_close();  // S51 cap
        const double pts=exit_px-pos_.entry;
        const double pnl=pts*pos_.lot*p.dollars_per_pt;
        printf("[XAU_OUTSIDE_BAR_D1] EXIT reason=%s entry=%.2f exit=%.2f pts=%.2f pnl=%.2f days=%d%s\n",
               reason, pos_.entry, exit_px, pts, pnl, pos_.days_held,
               shadow_mode ? " [SHADOW]" : "");
        fflush(stdout);
        TradeRecord tr{};
        tr.symbol="XAUUSD"; tr.side="LONG";
        tr.entryPrice=pos_.entry; tr.exitPrice=exit_px;
        tr.tp=pos_.tp; tr.sl=pos_.sl; tr.size=pos_.lot;
        tr.pnl=pnl; tr.mfe=pos_.mfe; tr.mae=pos_.mae;
        tr.entryTs=pos_.entry_ts_ms/1000; tr.exitTs=now_ms/1000;
        tr.exitReason=reason; tr.engine="XauOutsideBarD1";
        if (cb) cb(tr);
        pos_ = OpenPos{};
    }
};

} // namespace omega
