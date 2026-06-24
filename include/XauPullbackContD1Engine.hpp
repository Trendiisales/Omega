// =============================================================================
//  XauPullbackContD1Engine.hpp -- D1 pullback continuation (stronger than H4)
//
//  PROVENANCE (2026-05-20)
//
//  Mega-sweep daily variant of XauPullbackContH4Engine. Same archetype as
//  PullbackContEngine (retired S49 X5 Apr 26 2026), but D1 timeframe with
//  longer-lookback EMAs.
//
//  CONFIG (ema_fast=10, ema_slow=100, pullback_atr=1.0, hold=20, sl=1.5, tp=5.0):
//    Backtest 2yr XAU daily, IS/OOS robust + cost stress:
//      Cost 1bp:    FUL Sh=8.73, IS Sh=15.73, OOS Sh=6.90 (raw mega-sweep)
//      Cost 10bp:   FUL Sh=8.38
//      Cost 30bp:   FUL Sh=7.61
//      Cost 50bp:   FUL Sh=6.83
//      n=33, PnL=71.6%, WR=66.7%, mdd=7.2%
//
//  CAVEAT: IS Sh=15.73 vs OOS Sh=6.90 — IS half is stronger, but OOS still
//  excellent. ~16 trades/year on D1 = sparse.
//
//  S37 Phase H STAGE-TRAIL TOMBSTONE-BY-ANALOGY (2026-05-27b).
//  NOT empirically tested on this engine. Predicted NEGATIVE by analogy to
//  XauPullbackContH4 (same engine family, TP=5*ATR, trail -36% Sharpe on
//  xau_d1_zoo_audit). D1 timeframe has multi-day holds; trail-cut at +0.5N
//  would close winners before the multi-day pullback continuation resolves.
//  Trail STAYS OFF and is NOT implemented to keep diff small.
// =============================================================================

#pragma once
//  ADVERSE-PROTECTION: fixed SL/TP bracket (sl=1.5*ATR, tp=5.0*ATR, checked per-tick) + 20-day time-stop + profit-only weekend close; NO loss-cut/BE/trail by design -- trail tombstoned-by-analogy S37 Phase H (predicted -36% Sharpe vs XauPullbackContH4, never empirically tested) and engine is not wired live (no global instance; XauPullbackContD1 on CLAUDE.md DISABLED-must-gate list) -- no faithful backtest on record -- verdict owed before re-enable (backfill S-2026-06-24n)
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <algorithm>
#include "OmegaTradeLedger.hpp"
#include "PortfolioGuard.hpp"  // S51: concurrency cap

namespace omega {

struct XauPullbackContD1Params {
    int    ema_fast            = 10;
    int    ema_slow            = 100;
    double pullback_atr        = 1.0;
    int    hold_max_days       = 20;
    double sl_atr_mult         = 1.5;
    double tp_atr_mult         = 5.0;
    double lot                 = 0.01;
    double dollars_per_pt      = 1.0;
    int    atr_period          = 14;
    double max_spread          = 1.0;
    bool   weekend_close_gate  = true;
};

inline XauPullbackContD1Params make_xau_pullback_cont_d1_params() { return XauPullbackContD1Params{}; }

struct XauPullbackContD1Signal {
    bool        valid   = false;
    double      entry   = 0.0;
    double      sl      = 0.0;
    double      tp      = 0.0;
    double      lot     = 0.0;
    const char* reason  = "";
};

struct XauPullbackContD1Engine {
    bool   shadow_mode = true;
    bool   enabled     = true;
    XauPullbackContD1Params p;
    std::string symbol = "XAUUSD";

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;

    struct D1Accum {
        bool active=false; int64_t day_utc=0;
        double open=0, high=0, low=0, close=0;
    } d1_acc_;

    double ema_fast_=0, ema_slow_=0;
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

    XauPullbackContD1Signal on_h4_bar(double h4_high, double h4_low, double h4_close,
                                       double bid, double ask, int64_t h4_close_ms,
                                       CloseCallback on_close) noexcept {
        XauPullbackContD1Signal sig{};
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

            // Capture PRIOR state
            const double ema_fast_pre = (ema_count_ >= p.ema_fast) ? ema_fast_ : 0.0;
            const double ema_slow_pre = (ema_count_ >= p.ema_slow) ? ema_slow_ : 0.0;
            const double atr_pre = atr_;

            // Update EMAs
            const double af = 2.0/(p.ema_fast+1);
            const double as = 2.0/(p.ema_slow+1);
            if (ema_count_==0) { ema_fast_ = ema_slow_ = bar_close; }
            else {
                ema_fast_ = af*bar_close + (1-af)*ema_fast_;
                ema_slow_ = as*bar_close + (1-as)*ema_slow_;
            }
            ++ema_count_;

            _update_atr_on_bar_close(bar_high, bar_low, bar_close);
            ++bar_count_;

            if (!pos_.active && enabled
                && ema_count_ > p.ema_slow + 1
                && atr_pre > 0.0
                && ema_fast_pre > 0.0 && ema_slow_pre > 0.0
                && ema_fast_pre > ema_slow_pre
                && bar_low <= ema_fast_pre + p.pullback_atr * atr_pre
                && bar_close > ema_fast_pre
                && (ask - bid) <= p.max_spread
                && omega::pg::can_open_new_position())  // S51 cap
            {
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

                printf("[XAU_PULLBACK_CONT_D1] ENTRY LONG @ %.2f sl=%.2f tp=%.2f lot=%.2f"
                       " ema_f=%.2f ema_s=%.2f atr=%.2f%s\n",
                       entry_px, sl_px, tp_px, p.lot, ema_fast_pre, ema_slow_pre, atr_pre,
                       shadow_mode ? " [SHADOW]" : "");
                fflush(stdout);

                sig.valid=true; sig.entry=entry_px; sig.sl=sl_px; sig.tp=tp_px; sig.lot=p.lot;
                sig.reason = "XAU_PULLBACK_CONT_D1_LONG";
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
        omega::pg::register_position_close();  // S51 cap
        const double pts_move = exit_px - pos_.entry;
        const double pnl_dollars = pts_move * pos_.lot * p.dollars_per_pt;

        printf("[XAU_PULLBACK_CONT_D1] EXIT reason=%s entry=%.2f exit=%.2f pts=%.2f pnl=%.2f days=%d%s\n",
               reason, pos_.entry, exit_px, pts_move, pnl_dollars, pos_.days_held,
               shadow_mode ? " [SHADOW]" : "");
        fflush(stdout);

        TradeRecord tr{};
        tr.symbol="XAUUSD"; tr.side="LONG";
        tr.entryPrice=pos_.entry; tr.exitPrice=exit_px;
        tr.tp=pos_.tp; tr.sl=pos_.sl; tr.size=pos_.lot;
        tr.pnl=pnl_dollars; tr.mfe=pos_.mfe; tr.mae=pos_.mae;
        tr.entryTs=pos_.entry_ts_ms/1000; tr.exitTs=now_ms/1000;
        tr.exitReason=reason; tr.engine="XauPullbackContD1";
        if (on_close) on_close(tr);
        pos_ = OpenPos{};
    }
};

} // namespace omega
