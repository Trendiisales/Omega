// =============================================================================
//  Us303BarMomH1Engine.hpp -- US30 H1 three-bar momentum symmetric + MFE trail
//
//  PROVENANCE (2026-05-24)
//
//  /Users/jo/edge_research research. Symmetric long+short 3-bar momentum on
//  US30 H1 bars. 2yr backtest (2023-10 → 2025-10) on dow30_2yr.csv.
//  MFE-lock trail arm=1.0R lock=90%.
//
//  Validation:
//    27-month backtest H1: IS 161 trades PF 1.44 +$11,380 / DD -$4622
//      OOS 32 trades WR 53% PF 2.37 +$4489 DD -$682 Sharpe 1.91
//    Walk-forward 4 anchored folds:
//      f1 38n PF 1.45 +$2655 | f2 46n PF 1.10 +$736
//      f3 37n PF 1.32 +$2877 | f4 39n PF 1.96 +$4675
//    Aggregate OOS: 160 trades, net +$10,943, ALL folds positive
//
//  NOTE: backtest was LONG-only winners (US30 in uptrend 2023-2025). Symmetric
//  variant defends against regime change (cf. XAU L2 forward failure of
//  LONG-only 3bar_mom). Same caveat applies — if Dow tops, shorts should kick.
//
//  SIGNAL
//    LONG  if close[0] > close[-1] > close[-2] > close[-3]
//    SHORT if close[0] < close[-1] < close[-2] < close[-3]
//
//  EXIT
//    Hard SL: 3.0 × ATR(14) (wide stops — Dow swings)
//    Hard TP: 3R
//    Time:   96 H1 bars (~4 days)
//    Trail:  once MFE >= 1.0R, SL = entry + sign * 0.90 * (extreme - entry)
// =============================================================================

#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <fstream>
#include <functional>
#include <string>
#include "OmegaTradeLedger.hpp"
#include "SeedGuard.hpp"

namespace omega {

struct Us303BarMomH1Params {
    int    atr_period      = 14;
    double sl_atr_mult     = 3.0;
    double tp_r_mult       = 3.0;
    int    hold_max_bars   = 96;
    double trail_arm_R     = 1.0;
    double trail_lock_pct  = 0.90;
    double lot             = 0.01;
    double max_spread      = 3.0;   // US30 spread typical
    int    cooldown_bars   = 1;
};

struct Us303BarMomH1Signal {
    bool        valid  = false;
    int         side   = 0;
    double      entry  = 0.0;
    double      sl     = 0.0;
    double      tp     = 0.0;
    double      lot    = 0.0;
    const char* reason = "";
};

struct Us303BarMomH1Engine {
    bool shadow_mode = true;
    bool enabled     = false;
    Us303BarMomH1Params p;
    std::string symbol = "US30";

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;

    std::deque<double> closes_;
    double atr_ = 0.0;
    int    atr_seed_count_ = 0;
    double atr_seed_sum_   = 0.0;
    double prev_close_     = 0.0;
    int    bar_count_      = 0;
    int    cooldown_       = 0;

    struct OpenPos {
        bool active = false;
        int  side   = 0;
        double entry = 0, sl = 0, tp = 0, sl_dist = 0, lot = 0;
        double mfe = 0, extreme = 0;
        bool armed = false;
        int64_t entry_ts_ms = 0;
        int bars_held = 0;
    } pos_;

    int m_trade_id_ = 0;
    bool has_open_position() const noexcept { return pos_.active; }

    void _close(double exit_px, const char* reason, int64_t now_ms,
                CloseCallback cb) noexcept {
        if (!pos_.active) return;
        const double pnl = pos_.side * (exit_px - pos_.entry) * pos_.lot;
        omega::TradeRecord tr{};
        tr.symbol     = symbol;
        tr.side       = pos_.side > 0 ? "LONG" : "SHORT";
        tr.engine     = "Us303BarMomH1";
        tr.exitReason = reason;
        tr.entryPrice = pos_.entry;
        tr.exitPrice  = exit_px;
        tr.sl = pos_.sl; tr.tp = pos_.tp; tr.size = pos_.lot;
        tr.pnl = pnl;
        tr.entryTs = pos_.entry_ts_ms / 1000LL;
        tr.exitTs  = now_ms / 1000LL;
        tr.mfe = pos_.mfe;
        tr.atr_at_entry = pos_.sl_dist / std::max(p.sl_atr_mult, 1e-9);
        tr.shadow = shadow_mode;
        if (cb) cb(tr);
        printf("[US30_3BAR_MOM_H1] CLOSE %s @ %.2f entry=%.2f pnl=%.2f"
               " mfe=%.2f reason=%s bars=%d%s\n",
               pos_.side > 0 ? "LONG" : "SHORT",
               exit_px, pos_.entry, pnl, pos_.mfe, reason, pos_.bars_held,
               shadow_mode ? " [SHADOW]" : "");
        fflush(stdout);
        pos_ = OpenPos{};
        cooldown_ = p.cooldown_bars;
    }

    void _update_atr(double high, double low, double close) noexcept {
        if (prev_close_ <= 0.0) { prev_close_ = close; return; }
        const double tr = std::max({high - low,
                                    std::fabs(high - prev_close_),
                                    std::fabs(low  - prev_close_)});
        if (atr_seed_count_ < p.atr_period) {
            atr_seed_sum_ += tr;
            if (++atr_seed_count_ == p.atr_period)
                atr_ = atr_seed_sum_ / p.atr_period;
        } else {
            atr_ = (atr_ * (p.atr_period - 1) + tr) / p.atr_period;
        }
        prev_close_ = close;
    }

    void on_tick(double bid, double ask, int64_t now_ms,
                 CloseCallback cb) noexcept {
        if (!pos_.active || bid <= 0 || ask <= 0) return;
        const double mid = (bid + ask) * 0.5;
        const double move = pos_.side * (mid - pos_.entry);
        if (move > pos_.mfe) pos_.mfe = move;
        if (pos_.side > 0) { if (mid > pos_.extreme) pos_.extreme = mid; }
        else               { if (mid < pos_.extreme) pos_.extreme = mid; }
        if (!pos_.armed && pos_.sl_dist > 0.0) {
            if (move / pos_.sl_dist >= p.trail_arm_R) pos_.armed = true;
        }
        if (pos_.armed) {
            const double locked = pos_.entry + pos_.side *
                                  p.trail_lock_pct * (pos_.extreme - pos_.entry);
            if (pos_.side > 0) { if (locked > pos_.sl) pos_.sl = locked; }
            else               { if (locked < pos_.sl) pos_.sl = locked; }
        }
        if (pos_.side > 0) {
            if (bid <= pos_.sl)      _close(bid, pos_.armed ? "TRAIL_SL" : "SL_HIT", now_ms, cb);
            else if (bid >= pos_.tp) _close(bid, "TP_HIT", now_ms, cb);
        } else {
            if (ask >= pos_.sl)      _close(ask, pos_.armed ? "TRAIL_SL" : "SL_HIT", now_ms, cb);
            else if (ask <= pos_.tp) _close(ask, "TP_HIT", now_ms, cb);
        }
    }

    Us303BarMomH1Signal on_h1_bar(double h1_high, double h1_low, double h1_close,
                                   double bid, double ask, int64_t bar_close_ms,
                                   CloseCallback cb) noexcept {
        Us303BarMomH1Signal sig{};

        closes_.push_back(h1_close);
        while ((int)closes_.size() > 5) closes_.pop_front();
        _update_atr(h1_high, h1_low, h1_close);
        ++bar_count_;
        if (cooldown_ > 0) --cooldown_;

        if (pos_.active) {
            ++pos_.bars_held;
            if (pos_.bars_held >= p.hold_max_bars)
                _close((bid+ask)*0.5, "TIMEOUT", bar_close_ms, cb);
        }

        if (!enabled || pos_.active || cooldown_ > 0) return sig;
        if (atr_ <= 0.0) return sig;
        if ((ask - bid) > p.max_spread) return sig;
        if ((int)closes_.size() < 4) return sig;

        const int sz = (int)closes_.size();
        const double c0 = closes_[sz-1];
        const double c1 = closes_[sz-2];
        const double c2 = closes_[sz-3];
        const double c3 = closes_[sz-4];

        int side = 0;
        if (c0 > c1 && c1 > c2 && c2 > c3) side = +1;
        else if (c0 < c1 && c1 < c2 && c2 < c3) side = -1;
        if (side == 0) return sig;

        const double entry = (side > 0) ? ask : bid;
        const double sl_dist = p.sl_atr_mult * atr_;
        const double sl = entry - side * sl_dist;
        const double tp = entry + side * p.tp_r_mult * sl_dist;

        pos_.active = true; pos_.side = side;
        pos_.entry = entry; pos_.sl = sl; pos_.tp = tp; pos_.sl_dist = sl_dist;
        pos_.lot = p.lot; pos_.mfe = 0; pos_.extreme = entry; pos_.armed = false;
        pos_.entry_ts_ms = bar_close_ms; pos_.bars_held = 0;
        ++m_trade_id_;

        printf("[US30_3BAR_MOM_H1] ENTRY %s @ %.2f sl=%.2f tp=%.2f atr=%.2f%s\n",
               side > 0 ? "LONG" : "SHORT", entry, sl, tp, atr_,
               shadow_mode ? " [SHADOW]" : "");
        fflush(stdout);

        sig.valid = true; sig.side = side;
        sig.entry = entry; sig.sl = sl; sig.tp = tp; sig.lot = p.lot;
        sig.reason = side > 0 ? "US30_3BAR_MOM_H1_LONG" : "US30_3BAR_MOM_H1_SHORT";
        return sig;
    }

    // Warm-seed from H1 bar CSV. Format: ts,o,h,l,c (ts in SECONDS).
    size_t seed_from_h1_csv(const std::string& path) noexcept {
        const std::string actual = omega::resolve_seed_path(path);
        std::ifstream f(actual);
        if (!f.is_open()) omega::seed_die("Us303BarMomH1", actual);
        std::string line; std::getline(f, line);
        const bool was_enabled = enabled;
        enabled = false;
        auto null_cb = [](const omega::TradeRecord&){};
        size_t n = 0;
        while (std::getline(f, line)) {
            long long ts_s_ll = 0;
            double o=0, h=0, l=0, c=0;
            if (sscanf(line.c_str(), "%lld,%lf,%lf,%lf,%lf",
                       &ts_s_ll, &o, &h, &l, &c) == 5) {
                const double sp = 1.5;  // synthetic US30 spread
                const double bid = c - sp/2, ask = c + sp/2;
                on_h1_bar(h, l, c, bid, ask, static_cast<int64_t>(ts_s_ll)*1000LL, null_cb);
                ++n;
            }
        }
        enabled = was_enabled;
        if (n == 0) omega::seed_die("Us303BarMomH1", actual);
        printf("[SEED] Us303BarMomH1: %zu H1 bars replayed from %s -- hot"
               " (atr=%.2f closes_buf=%d)\n",
               n, actual.c_str(), atr_, (int)closes_.size());
        fflush(stdout);
        return n;
    }
};

} // namespace omega
