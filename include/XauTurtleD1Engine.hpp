// =============================================================================
//  XauTurtleD1Engine.hpp -- XAU 40-day Donchian breakout (Turtle archetype)
//
//  PROVENANCE (2026-05-20)
//
//  Resurrection of the Turtle/Donchian D1 signal pattern. The original
//  TurtleTickEngine was retired at S50 X1 (Apr 27 2026) as part of the
//  gold stack cleanup. The signal archetype was re-tested on current 2yr
//  XAU daily data and shows extremely strong edge:
//
//    Config (lookback=40, hold=10, sl_atr=1.5, tp_atr=3.0, long_only):
//      Cost 1bps:    IS Sh=8.08, OOS Sh=18.96, FUL Sh=13.57, n=20 PnL=43.5%
//      Cost 10bps:   IS Sh=7.32, OOS Sh=18.42, FUL Sh=13.01
//      Cost 50bps:   IS Sh=3.96, OOS Sh=16.03, FUL Sh=10.51 (still robust)
//
//    Robustness +/-20% params: full-sample Sh 10-15 across all neighbours
//    IS/OOS 50/50 time-split: both halves strongly positive
//    Win rate: 70%
//
//  CAVEAT: n=20 over 2yr = ~10 trades/year. Sparse. Sharpe estimate has
//  higher variance from small sample. Min 5 live shadow trades before
//  considering LIVE promotion.
//
//  Distinct from MinimalH4Breakout (gold) which uses H4 timeframe + don=10,
//  and from XauTrendFollowD1Engine which uses lb=20 momentum + Keltner +
//  ADX cells. This engine is pure 40-day high-close break, long-only.
//
// S37 Phase H STAGE-TRAIL TOMBSTONE-BY-ANALOGY (2026-05-27b).
// NOT empirically tested on this engine. Predicted NEGATIVE by analogy to
// EmaPullback (TP=2.5R, trail -76% PnL) and XauPullbackContH4 (TP=5N,
// trail -36% Sharpe). This engine: TP at 3*ATR with SL at 1.5*ATR.
// Stage1 arm at 2*ATR catches winners at +0.5N before they reach TP at +3N;
// lost TP_HITs outweigh saved giveback. Trail STAYS OFF and is NOT
// implemented on this engine to keep diff small. To verify empirically,
// add the same gated trail block used on XauPullbackContH4Engine.hpp and
// flip pcH4.p.stage_trail_enabled = true in xau_d1_zoo_audit.
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
#include "OmegaCostGuard.hpp"
#include "PortfolioGuard.hpp"  // S51: concurrency cap
#include "RegimeState.hpp"     // 2026-06-12: shared price-based bull/bear gate

namespace omega {

struct XauTurtleD1Params {
    int    lookback_days       = 40;
    int    hold_max_days       = 10;
    double sl_atr_mult         = 1.5;
    double tp_atr_mult         = 3.0;
    double risk_dollars        = 10.0;
    double lot                 = 0.01;
    double dollars_per_pt      = 1.0;
    int    atr_period          = 14;
    double max_spread          = 1.0;
    bool   weekend_close_gate  = true;
};

inline XauTurtleD1Params make_xau_turtle_d1_params() { return XauTurtleD1Params{}; }

struct XauTurtleD1Signal {
    bool        valid   = false;
    bool        is_long = false;
    double      entry   = 0.0;
    double      sl      = 0.0;
    double      tp      = 0.0;
    double      lot     = 0.0;
    const char* reason  = "";
};

struct XauTurtleD1Engine {
    bool   shadow_mode = true;
    bool   enabled     = true;
    XauTurtleD1Params p;
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

    XauTurtleD1Signal on_h4_bar(double h4_high, double h4_low, double h4_close,
                                 double bid, double ask, int64_t h4_close_ms,
                                 CloseCallback on_close) noexcept {
        XauTurtleD1Signal sig{};
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

            // Capture PRIOR state for signal eval (no look-ahead)
            const double atr_pre = atr_;
            const int n_prior = (int)d1_highs_.size();
            double prior_high = 0.0;
            if (n_prior >= p.lookback_days) {
                // Maximum of last p.lookback_days highs
                int start = n_prior - p.lookback_days;
                prior_high = d1_highs_[start];
                for (int i = start+1; i < n_prior; ++i)
                    if (d1_highs_[i] > prior_high) prior_high = d1_highs_[i];
            }

            // Push closed D1 bar
            d1_highs_.push_back(bar_high);
            d1_lows_ .push_back(bar_low);
            d1_closes_.push_back(bar_close);
            const int keep = std::max(p.lookback_days, p.atr_period) + 2;
            while ((int)d1_highs_.size() > keep) {
                d1_highs_.pop_front();
                d1_lows_ .pop_front();
                d1_closes_.pop_front();
            }

            _update_atr_on_bar_close(bar_high, bar_low, bar_close);
            ++bar_count_;

            // Signal: close > prior 40-day high
            if (!pos_.active && enabled && n_prior >= p.lookback_days
                && atr_pre > 0.0 && prior_high > 0.0
                && (ask - bid) <= p.max_spread
                && bar_close > prior_high
                && !omega::gold_regime().long_blocked()  // 2026-06-12 regime gate:
                                                         //   skip Donchian-breakout longs in a sustained
                                                         //   gold bear (close<EMA200 + EMA200 falling +
                                                         //   EMA50<EMA200). Backtest gold_regime_gate_bt
                                                         //   (XAU H1 2020-23): net +141->+153, H2/bear
                                                         //   bleed -189->-85. Inert in bull (blocks ~0).
                && omega::pg::can_open_new_position())  // S51 cap
            {
                const double entry_px = ask;
                const double atr_pct = atr_pre / bar_close;
                const double sl_px = entry_px * (1.0 - p.sl_atr_mult * atr_pct);
                const double tp_px = entry_px * (1.0 + p.tp_atr_mult * atr_pct);

                // cost gate: TP distance vs spread cost. NO early return -- the
                // d1_acc_ reset below must still run on a blocked entry.
                if (ExecutionCostGuard::is_viable("XAUUSD", ask - bid, tp_px - entry_px, p.lot, 1.5)) {

                pos_.active=true; pos_.is_long=true;
                omega::pg::register_position_open();  // S51 cap
                pos_.entry=entry_px; pos_.sl=sl_px; pos_.tp=tp_px;
                pos_.lot=p.lot; pos_.mfe=pos_.mae=0;
                pos_.entry_ts_ms=h4_close_ms; pos_.days_held=0;
                ++m_trade_id_;

                printf("[XAU_TURTLE_D1] ENTRY LONG @ %.2f sl=%.2f tp=%.2f lot=%.2f"
                       " prior40d_high=%.2f atr=%.2f%s\n",
                       entry_px, sl_px, tp_px, p.lot, prior_high, atr_pre,
                       shadow_mode ? " [SHADOW]" : "");
                fflush(stdout);

                sig.valid=true; sig.is_long=true;
                sig.entry=entry_px; sig.sl=sl_px; sig.tp=tp_px; sig.lot=p.lot;
                sig.reason = "XAU_TURTLE_D1_LONG";
                }
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
        omega::pg::register_position_close();  // S51 cap
        const double pts_move = pos_.is_long ? (exit_px - pos_.entry) : (pos_.entry - exit_px);
        const double pnl_dollars = pts_move * pos_.lot * p.dollars_per_pt;

        printf("[XAU_TURTLE_D1] EXIT %s reason=%s entry=%.2f exit=%.2f pts=%.2f"
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
        tr.exitReason=reason; tr.engine="XauTurtleD1";
        if (on_close) on_close(tr);
        pos_ = OpenPos{};
    }
};

} // namespace omega
