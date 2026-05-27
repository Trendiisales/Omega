// =============================================================================
//  XauTsmomFastD1Engine.hpp -- XAU short-lookback daily time-series momentum
//
//  PROVENANCE (2026-05-20)
//
//  Built after audit of existing XauTrendFollowD1Engine (S33e). That engine
//  uses lookback=20 momentum cell. This engine targets the OTHER pocket of
//  D1 trend edge found via tsmom_gold_probe.py: short-lookback (3-5 day)
//  momentum with tight SL + wide TP.
//
//  CONFIG (lb=5 h=20 sl=1.0 tp=5.0 long_only) on 2yr daily XAUUSD bars:
//    Cost stress (cost_pct of entry price, round-trip):
//      1bps:   Full Sh=7.57, IS Sh=6.69, OOS Sh=7.65  (n=48, PnL=78.1%)
//      3bps:   Full Sh=7.48
//      5bps:   Full Sh=7.39
//      10bps:  Full Sh=7.15
//      20bps:  Full Sh=6.69
//    n=48 over 670 daily bars (~2 trades/month).
//    IS/OOS 50/50 split: both halves Sharpe > 6.
//
//  WHY NOT JUST ADD A CELL TO XauTrendFollowD1?
//    Existing D1 engine has specific cell struct/ensemble logic. Adding a
//    cell requires deep familiarity with that file. Standalone engine here
//    keeps the code blast radius minimal and shadow-mode validation isolated.
//    Can fold into the D1 ensemble later if both engines validate side-by-side.
//
//  ARCHITECTURE
//    Self-contained: builds own D1 OHLC + ATR14 from H4 closes (mirrors
//    XauTrendFollowD1Engine's H4-aggregation approach). on_h4_bar()
//    accumulates into a D1 bar; UTC day rollover closes the D1 bar and
//    triggers signal eval.
//
//  SAFETY
//    - shadow_mode=true default. NEVER set false without n>=10 live trades.
//    - 0.01 lot fixed.
//    - Long-only. No bear-side trades (2024-2026 gold uptrend regime).
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

struct XauTsmomFastD1Params {
    int    lookback_days       = 5;     // momentum lookback
    int    hold_max_days       = 20;    // safety timeout
    double sl_atr_mult         = 1.0;   // SL = entry * (1 - sl_atr_mult * atr_pct)
    double tp_atr_mult         = 5.0;   // TP = entry * (1 + tp_atr_mult * atr_pct)
    double min_mom_pct         = 0.005; // require |log-return| >= 0.5% to enter
    double risk_dollars        = 10.0;
    double lot                 = 0.01;
    double dollars_per_pt      = 1.0;   // XAU $1/pt at 0.01 lot (cost_calibrator)
    int    atr_period          = 14;
    double max_spread          = 1.0;   // XAU spread typically $0.20-0.50
    bool   weekend_close_gate  = true;
};

inline XauTsmomFastD1Params make_xau_tsmom_fast_d1_params() { return XauTsmomFastD1Params{}; }

struct XauTsmomFastD1Signal {
    bool        valid   = false;
    bool        is_long = false;
    double      entry   = 0.0;
    double      sl      = 0.0;
    double      tp      = 0.0;
    double      lot     = 0.0;
    const char* reason  = "";
};

struct XauTsmomFastD1Engine {

    bool   shadow_mode = true;
    bool   enabled     = true;
    XauTsmomFastD1Params p;
    std::string symbol = "XAUUSD";

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;

    // ── D1 OHLC accumulator (built from H4 close events) ─────────────────────
    struct D1Accum {
        bool     active     = false;
        int64_t  day_utc    = 0;
        double   open       = 0.0;
        double   high       = 0.0;
        double   low        = 0.0;
        double   close      = 0.0;
    } d1_acc_;

    // ── Closed D1 history (close prices for momentum + ATR true range) ──────
    std::deque<double> d1_closes_;
    std::deque<double> d1_highs_;
    std::deque<double> d1_lows_;

    // Wilder ATR14 state
    double  atr_                = 0.0;
    int     atr_seed_count_     = 0;
    double  atr_seed_sum_       = 0.0;
    double  prev_d1_close_      = 0.0;

    int     bar_count_          = 0;

    struct OpenPos {
        bool    active        = false;
        bool    is_long       = false;
        double  entry         = 0.0;
        double  sl            = 0.0;
        double  tp            = 0.0;
        double  lot           = 0.0;
        double  mfe           = 0.0;
        double  mae           = 0.0;
        int64_t entry_ts_ms   = 0;
        int     days_held     = 0;
    } pos_;

    int m_trade_id_ = 0;

    bool has_open_position() const noexcept { return pos_.active; }

    // ── on_tick: manage open position (SL/TP on every tick) ─────────────────
    void on_tick(double bid, double ask, int64_t now_ms, CloseCallback on_close) noexcept {
        if (!pos_.active || bid <= 0.0 || ask <= 0.0) return;
        const double mid = (bid + ask) * 0.5;
        const double move = pos_.is_long ? (mid - pos_.entry) : (pos_.entry - mid);
        if (move > pos_.mfe) pos_.mfe = move;
        if (move < pos_.mae) pos_.mae = move;
        if (pos_.is_long) {
            if (bid <= pos_.sl)      _close(bid, "SL_HIT", now_ms, on_close);
            else if (bid >= pos_.tp) _close(bid, "TP_HIT", now_ms, on_close);
        } else {
            if (ask >= pos_.sl)      _close(ask, "SL_HIT", now_ms, on_close);
            else if (ask <= pos_.tp) _close(ask, "TP_HIT", now_ms, on_close);
        }
    }

    // ── on_h4_bar: build D1 bars from H4 closes ─────────────────────────────
    // Called from tick_gold.hpp on each H4 close. We aggregate H4 closes into
    // D1 OHLC and emit the D1 close signal when the UTC day rolls over.
    XauTsmomFastD1Signal on_h4_bar(double h4_high, double h4_low, double h4_close,
                                    double bid, double ask, int64_t h4_close_ms,
                                    CloseCallback on_close) noexcept
    {
        XauTsmomFastD1Signal sig{};
        const int64_t day_utc = h4_close_ms / 86400000LL;

        if (!d1_acc_.active) {
            d1_acc_.active = true;
            d1_acc_.day_utc = day_utc;
            d1_acc_.open = h4_close;
            d1_acc_.high = h4_high;
            d1_acc_.low  = h4_low;
            d1_acc_.close = h4_close;
            return sig;
        }

        if (day_utc != d1_acc_.day_utc) {
            // D1 boundary crossed -- close the day's accumulated bar
            const double bar_high = d1_acc_.high;
            const double bar_low  = d1_acc_.low;
            const double bar_close = d1_acc_.close;

            // Capture state BEFORE updating (matches backtest: signal uses
            // prior closes + prior ATR).
            const double atr_pre = atr_;
            const int closes_n_pre = (int)d1_closes_.size();

            // Push closed D1 bar
            d1_closes_.push_back(bar_close);
            d1_highs_ .push_back(bar_high);
            d1_lows_  .push_back(bar_low);
            const int keep = std::max(p.lookback_days, p.atr_period) + 2;
            while ((int)d1_closes_.size() > keep) {
                d1_closes_.pop_front();
                d1_highs_ .pop_front();
                d1_lows_  .pop_front();
            }

            // Update ATR14 with this bar's TR
            _update_atr_on_bar_close(bar_high, bar_low, bar_close);
            ++bar_count_;

            // Signal eval: need lookback_days+1 prior closes + ATR ready
            if (!pos_.active && enabled
                && closes_n_pre >= p.lookback_days + 1
                && atr_pre > 0.0
                && (ask - bid) <= p.max_spread)
            {
                // Past close = d1_closes_[size-1-lookback_days] (size includes this bar)
                const int sz = (int)d1_closes_.size();
                const double past_close = d1_closes_[sz - 1 - p.lookback_days];
                const double mom = std::log(bar_close / past_close);

                // Long-only: require positive momentum above threshold
                if (mom >= p.min_mom_pct && omega::pg::can_open_new_position()) {  // S51 cap
                    const double entry_px = ask;
                    const double atr_pct  = atr_pre / bar_close;
                    const double sl_px    = entry_px * (1.0 - p.sl_atr_mult * atr_pct);
                    const double tp_px    = entry_px * (1.0 + p.tp_atr_mult * atr_pct);

                    pos_.active      = true;
                    omega::pg::register_position_open();  // S51 cap
                    pos_.is_long     = true;
                    pos_.entry       = entry_px;
                    pos_.sl          = sl_px;
                    pos_.tp          = tp_px;
                    pos_.lot         = p.lot;
                    pos_.mfe         = 0.0;
                    pos_.mae         = 0.0;
                    pos_.entry_ts_ms = h4_close_ms;
                    pos_.days_held   = 0;
                    ++m_trade_id_;

                    printf("[XAU_TSMOM_FAST_D1] ENTRY LONG @ %.2f sl=%.2f tp=%.2f lot=%.2f"
                           " mom=%.4f atr=%.2f%s\n",
                           entry_px, sl_px, tp_px, p.lot, mom, atr_pre,
                           shadow_mode ? " [SHADOW]" : "");
                    fflush(stdout);

                    sig.valid = true; sig.is_long = true;
                    sig.entry = entry_px; sig.sl = sl_px; sig.tp = tp_px;
                    sig.lot = p.lot;
                    sig.reason = "XAU_TSMOM_FAST_D1_LONG";
                }
            }

            // Manage open-position day counter + timeout
            if (pos_.active) {
                ++pos_.days_held;
                if (pos_.days_held >= p.hold_max_days) {
                    _close(pos_.is_long ? bid : ask, "TIMEOUT", h4_close_ms, on_close);
                }
            }

            // Start new D1 accumulator
            d1_acc_.day_utc = day_utc;
            d1_acc_.open = h4_close;
            d1_acc_.high = h4_high;
            d1_acc_.low  = h4_low;
            d1_acc_.close = h4_close;
        } else {
            // Same UTC day -- extend the running D1 bar with H4 high/low/close
            if (h4_high > d1_acc_.high) d1_acc_.high = h4_high;
            if (h4_low  < d1_acc_.low)  d1_acc_.low  = h4_low;
            d1_acc_.close = h4_close;
        }
        return sig;
    }

    void check_weekend_close(double bid, double ask, int64_t now_ms,
                             CloseCallback on_close) noexcept
    {
        if (!pos_.active || !p.weekend_close_gate) return;
        const int64_t utc_sec = now_ms / 1000LL;
        const int dow = static_cast<int>((utc_sec / 86400LL + 3) % 7);
        const int hour = static_cast<int>((utc_sec % 86400LL) / 3600LL);
        if (dow != 4 || hour < 20) return;
        const double mid = (bid + ask) * 0.5;
        const double move = pos_.is_long ? (mid - pos_.entry) : (pos_.entry - mid);
        if (move > 0.0) {
            _close(pos_.is_long ? bid : ask, "WEEKEND_CLOSE", now_ms, on_close);
        }
    }

    void force_close(double bid, double ask, int64_t now_ms, CloseCallback cb) noexcept {
        if (!pos_.active) return;
        _close(pos_.is_long ? bid : ask, "FORCE_CLOSE", now_ms, cb);
    }

    void cancel() noexcept { pos_ = OpenPos{}; }

    // ── Internals ───────────────────────────────────────────────────────────
    void _update_atr_on_bar_close(double bar_h, double bar_l, double bar_c) noexcept {
        double tr = bar_h - bar_l;
        if (prev_d1_close_ > 0.0) {
            tr = std::max(tr, std::fabs(bar_h - prev_d1_close_));
            tr = std::max(tr, std::fabs(bar_l - prev_d1_close_));
        }
        prev_d1_close_ = bar_c;
        if (atr_seed_count_ < p.atr_period) {
            atr_seed_sum_ += tr;
            ++atr_seed_count_;
            if (atr_seed_count_ == p.atr_period) atr_ = atr_seed_sum_ / p.atr_period;
        } else {
            atr_ = (atr_ * (p.atr_period - 1) + tr) / p.atr_period;
        }
    }

    void _close(double exit_px, const char* reason, int64_t now_ms,
                CloseCallback on_close) noexcept
    {
        if (!pos_.active) return;
        omega::pg::register_position_close();  // S51 cap
        const double pts_move = pos_.is_long ? (exit_px - pos_.entry) : (pos_.entry - exit_px);
        const double pnl_dollars = pts_move * pos_.lot * p.dollars_per_pt;

        printf("[XAU_TSMOM_FAST_D1] EXIT %s reason=%s entry=%.2f exit=%.2f pts=%.2f"
               " pnl=%.2f days=%d%s\n",
               pos_.is_long ? "LONG" : "SHORT", reason, pos_.entry, exit_px,
               pts_move, pnl_dollars, pos_.days_held,
               shadow_mode ? " [SHADOW]" : "");
        fflush(stdout);

        TradeRecord tr{};
        tr.symbol     = symbol;
        tr.side       = pos_.is_long ? "LONG" : "SHORT";
        tr.entryPrice = pos_.entry;
        tr.exitPrice  = exit_px;
        tr.tp         = pos_.tp;
        tr.sl         = pos_.sl;
        tr.size       = pos_.lot;
        tr.pnl        = pnl_dollars;
        tr.mfe        = pos_.mfe;
        tr.mae        = pos_.mae;
        tr.entryTs    = pos_.entry_ts_ms / 1000;
        tr.exitTs     = now_ms / 1000;
        tr.exitReason = reason;
        tr.engine     = "XauTsmomFastD1";
        if (on_close) on_close(tr);

        pos_ = OpenPos{};
    }
};

} // namespace omega
