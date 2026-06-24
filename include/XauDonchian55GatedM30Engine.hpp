// =============================================================================
//  XauDonchian55GatedM30Engine.hpp -- XAU M30 Donchian-55 symmetric + regime gate
//
//  PROVENANCE (2026-05-24)
//
//  /Users/jo/edge_research research (FINAL_REPORT.md). Symmetric Donchian-55
//  breakout, EMA50/200 trend gate, MFE-lock trail (arm=0.7R, lock=80%).
//
//  Validation:
//    27-month backtest (2024-03 → 2026-04): IS PF 1.10 +$460 / OOS PF 1.67 +$1556
//      / DD -$695  (with trail; without trail OOS +$3035 DD -$908)
//    L2 forward (2026-04-09 → 2026-05-19): PF 3.03 +$773 DD -$200 / 48 trades
//    Walk-forward 4 anchored folds: every fold positive, OOS aggregate +$2127
//
//  SIGNAL
//    LONG  if uptrend gate AND close > prior 55-bar high
//    SHORT if downtrend gate AND close < prior 55-bar low
//    Gate: uptrend   = EMA50 > EMA200 AND EMA50 > EMA50[-20]
//          downtrend = EMA50 < EMA200 AND EMA50 < EMA50[-20]
//
//  EXIT
//    Hard SL: 1.5 × ATR(14) from entry
//    Hard TP: 5 × initial SL distance (5R)
//    Time:   48 M30 bars (24h) max hold
//    Trail:  once MFE >= 0.7R, SL = entry + sign * 0.80 * (extreme - entry)
//
//  SAFETY
//    shadow_mode = true by default
//    Cooldown 1 M30 bar after each trade close
//    Spread cap 1.0 USD
//    No symbol-config or core-file modifications
// =============================================================================

#pragma once
//  ADVERSE-PROTECTION: HAS full bracket -- hard SL 1.5xATR + 5R TP + 48-bar time-stop + MFE-lock trail (arm 0.7R, lock 80%); but engine is DISABLED (enabled=false) per engine_init.hpp S50 2026-05-27 real-class audit xau_donchian55_m30_audit (Sharpe -1.47 / -$14 / 585 trades / 22:1 SL:TP, L2-forward "PF 3.03" was inline-reimpl inflation) -- protection moot while off, verdict owed before re-enable (backfill S-2026-06-24n)
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

struct XauDonchian55GatedM30Params {
    int    donchian_period = 55;
    int    ema_fast        = 50;
    int    ema_slow        = 200;
    int    ema_slope_lb    = 20;
    int    atr_period      = 14;
    double sl_atr_mult     = 1.5;
    double tp_r_mult       = 5.0;
    int    hold_max_bars   = 48;
    double trail_arm_R     = 0.7;
    double trail_lock_pct  = 0.80;
    double lot             = 0.01;
    double max_spread      = 1.0;
    int    cooldown_bars   = 1;
};

struct XauDonchian55GatedM30Signal {
    bool        valid  = false;
    int         side   = 0;       // +1 long, -1 short
    double      entry  = 0.0;
    double      sl     = 0.0;
    double      tp     = 0.0;
    double      lot    = 0.0;
    const char* reason = "";
};

struct XauDonchian55GatedM30Engine {
    bool   shadow_mode = true;
    bool   enabled     = false;
    XauDonchian55GatedM30Params p;
    std::string symbol = "XAUUSD";

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;

    // M30 bar history: keep enough for donchian + ema_slow + slope
    std::deque<double> m30_highs_;
    std::deque<double> m30_lows_;
    std::deque<double> m30_closes_;
    std::deque<double> ema_fast_hist_;  // for slope check

    // EMA state
    double ema_fast_  = 0.0;
    double ema_slow_  = 0.0;
    int    ema_count_ = 0;

    // ATR state (Wilder-style rolling average of TR)
    double atr_      = 0.0;
    int    atr_seed_count_ = 0;
    double atr_seed_sum_   = 0.0;
    double prev_close_     = 0.0;

    int    bar_count_  = 0;
    int    cooldown_   = 0;

    struct OpenPos {
        bool active = false;
        int  side   = 0;
        double entry = 0, sl = 0, tp = 0, sl_dist = 0, lot = 0;
        double mfe = 0;        // best price in favourable direction
        double extreme = 0;    // running extreme
        bool armed = false;    // trail armed (MFE crossed arm_R)
        int64_t entry_ts_ms = 0;
        int bars_held = 0;
    } pos_;

    int m_trade_id_ = 0;

    bool has_open_position() const noexcept { return pos_.active; }

    // ---------- helpers ----------

    void _close(double exit_px, const char* reason, int64_t now_ms,
                CloseCallback cb) noexcept {
        if (!pos_.active) return;
        const double pnl = pos_.side * (exit_px - pos_.entry) * pos_.lot;
        omega::TradeRecord tr{};
        tr.symbol     = symbol;
        tr.side       = pos_.side > 0 ? "LONG" : "SHORT";
        tr.engine     = "XauDonchian55GatedM30";
        tr.exitReason = reason;
        tr.entryPrice = pos_.entry;
        tr.exitPrice  = exit_px;
        tr.sl         = pos_.sl;
        tr.tp         = pos_.tp;
        tr.size       = pos_.lot;
        tr.pnl        = pnl;
        tr.entryTs    = pos_.entry_ts_ms / 1000LL;
        tr.exitTs     = now_ms / 1000LL;
        tr.mfe        = pos_.mfe;
        tr.atr_at_entry = pos_.sl_dist / std::max(p.sl_atr_mult, 1e-9);
        tr.shadow     = shadow_mode;
        if (cb) cb(tr);
        printf("[XAU_D55_GATED_M30] CLOSE %s %s @ %.2f entry=%.2f pnl=%.2f"
               " mfe=%.2f reason=%s bars=%d%s\n",
               symbol.c_str(), pos_.side>0 ? "LONG" : "SHORT",
               exit_px, pos_.entry, pnl, pos_.mfe, reason, pos_.bars_held,
               shadow_mode ? " [SHADOW]" : "");
        fflush(stdout);
        pos_ = OpenPos{};
        cooldown_ = p.cooldown_bars;
    }

    void _update_emas(double close) noexcept {
        const double kf = 2.0 / (p.ema_fast + 1);
        const double ks = 2.0 / (p.ema_slow + 1);
        if (ema_count_ == 0) {
            ema_fast_ = close;
            ema_slow_ = close;
        } else {
            ema_fast_ = close * kf + ema_fast_ * (1.0 - kf);
            ema_slow_ = close * ks + ema_slow_ * (1.0 - ks);
        }
        ++ema_count_;
        ema_fast_hist_.push_back(ema_fast_);
        const int keep = p.ema_slope_lb + 2;
        while ((int)ema_fast_hist_.size() > keep) ema_fast_hist_.pop_front();
    }

    void _update_atr_on_bar(double high, double low, double close) noexcept {
        if (prev_close_ <= 0.0) { prev_close_ = close; return; }
        const double tr = std::max({high - low,
                                    std::fabs(high - prev_close_),
                                    std::fabs(low  - prev_close_)});
        if (atr_seed_count_ < p.atr_period) {
            atr_seed_sum_ += tr;
            ++atr_seed_count_;
            if (atr_seed_count_ == p.atr_period)
                atr_ = atr_seed_sum_ / p.atr_period;
        } else {
            atr_ = (atr_ * (p.atr_period - 1) + tr) / p.atr_period;
        }
        prev_close_ = close;
    }

    bool _uptrend() const noexcept {
        if ((int)ema_fast_hist_.size() <= p.ema_slope_lb) return false;
        const double ema_past = ema_fast_hist_[ema_fast_hist_.size() - 1 - p.ema_slope_lb];
        return (ema_fast_ > ema_slow_) && (ema_fast_ > ema_past);
    }
    bool _downtrend() const noexcept {
        if ((int)ema_fast_hist_.size() <= p.ema_slope_lb) return false;
        const double ema_past = ema_fast_hist_[ema_fast_hist_.size() - 1 - p.ema_slope_lb];
        return (ema_fast_ < ema_slow_) && (ema_fast_ < ema_past);
    }

    // ---------- tick path: manage open position (SL/TP/trail) ----------
    void on_tick(double bid, double ask, int64_t now_ms,
                 CloseCallback cb) noexcept {
        if (!pos_.active || bid <= 0 || ask <= 0) return;
        const double mid = (bid + ask) * 0.5;
        const double move = pos_.side * (mid - pos_.entry);
        if (move > pos_.mfe) pos_.mfe = move;
        // track extreme price in favour
        if (pos_.side > 0) {
            if (mid > pos_.extreme) pos_.extreme = mid;
        } else {
            if (mid < pos_.extreme) pos_.extreme = mid;
        }
        // arm trail when MFE crosses arm_R
        if (!pos_.armed && pos_.sl_dist > 0.0) {
            const double mfe_R = move / pos_.sl_dist;
            if (mfe_R >= p.trail_arm_R) pos_.armed = true;
        }
        // update trail SL: lock pct of extreme excursion
        if (pos_.armed) {
            const double locked = pos_.entry + pos_.side *
                                  p.trail_lock_pct * (pos_.extreme - pos_.entry);
            if (pos_.side > 0) { if (locked > pos_.sl) pos_.sl = locked; }
            else               { if (locked < pos_.sl) pos_.sl = locked; }
        }
        // exit check
        if (pos_.side > 0) {
            if (bid <= pos_.sl)      _close(bid, pos_.armed ? "TRAIL_SL" : "SL_HIT", now_ms, cb);
            else if (bid >= pos_.tp) _close(bid, "TP_HIT", now_ms, cb);
        } else {
            if (ask >= pos_.sl)      _close(ask, pos_.armed ? "TRAIL_SL" : "SL_HIT", now_ms, cb);
            else if (ask <= pos_.tp) _close(ask, "TP_HIT", now_ms, cb);
        }
    }

    // ---------- M30 bar close: signal + entry ----------
    XauDonchian55GatedM30Signal on_m30_bar(double m30_high, double m30_low,
                                            double m30_close, double bid,
                                            double ask, int64_t bar_close_ms,
                                            CloseCallback cb) noexcept {
        XauDonchian55GatedM30Signal sig{};

        // Update indicators on this just-closed bar
        m30_highs_.push_back(m30_high);
        m30_lows_ .push_back(m30_low);
        m30_closes_.push_back(m30_close);
        const int keep = std::max({p.donchian_period + 2, p.ema_slow + p.ema_slope_lb + 2});
        while ((int)m30_highs_.size() > keep) {
            m30_highs_.pop_front();
            m30_lows_ .pop_front();
            m30_closes_.pop_front();
        }
        _update_atr_on_bar(m30_high, m30_low, m30_close);
        _update_emas(m30_close);
        ++bar_count_;
        if (cooldown_ > 0) --cooldown_;

        // position management on bar close
        if (pos_.active) {
            ++pos_.bars_held;
            if (pos_.bars_held >= p.hold_max_bars)
                _close((bid+ask)*0.5, "TIMEOUT", bar_close_ms, cb);
        }

        if (!enabled || pos_.active || cooldown_ > 0) return sig;
        if (atr_ <= 0.0) return sig;
        if ((ask - bid) > p.max_spread) return sig;
        if ((int)m30_highs_.size() < p.donchian_period + 1) return sig;

        // donchian channel (prior N bars, EXCLUDING current bar)
        const int sz = (int)m30_highs_.size();
        double prior_high = m30_highs_[sz - 1 - p.donchian_period];
        double prior_low  = m30_lows_[sz - 1 - p.donchian_period];
        for (int i = sz - p.donchian_period; i < sz - 1; ++i) {
            if (m30_highs_[i] > prior_high) prior_high = m30_highs_[i];
            if (m30_lows_[i]  < prior_low)  prior_low  = m30_lows_[i];
        }

        const bool up   = _uptrend();
        const bool down = _downtrend();
        int side = 0;
        if (up   && m30_close > prior_high) side = +1;
        if (down && m30_close < prior_low)  side = -1;
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

        printf("[XAU_D55_GATED_M30] ENTRY %s @ %.2f sl=%.2f tp=%.2f atr=%.2f"
               " prior_hi=%.2f prior_lo=%.2f%s\n",
               side > 0 ? "LONG" : "SHORT", entry, sl, tp, atr_,
               prior_high, prior_low, shadow_mode ? " [SHADOW]" : "");
        fflush(stdout);

        sig.valid = true; sig.side = side;
        sig.entry = entry; sig.sl = sl; sig.tp = tp; sig.lot = p.lot;
        sig.reason = side > 0 ? "XAU_D55_GATED_M30_LONG" : "XAU_D55_GATED_M30_SHORT";
        return sig;
    }

    // ---------- warm seed from M30 bar CSV ----------
    // CSV format: bar_start_ms,open,high,low,close
    size_t seed_from_m30_csv(const std::string& path) noexcept {
        const std::string actual = omega::resolve_seed_path(path);
        std::ifstream f(actual);
        if (!f.is_open()) {
            omega::seed_die("XauDonchian55GatedM30", actual);
        }
        std::string line; std::getline(f, line);  // header
        const bool was_enabled = enabled;
        enabled = false;
        auto null_cb = [](const omega::TradeRecord&){};
        size_t n = 0;
        while (std::getline(f, line)) {
            long long ts_ms_ll = 0;
            double o=0, h=0, l=0, c=0;
            if (sscanf(line.c_str(), "%lld,%lf,%lf,%lf,%lf",
                       &ts_ms_ll, &o, &h, &l, &c) == 5) {
                const double bid = c - 0.15, ask = c + 0.15;
                on_m30_bar(h, l, c, bid, ask, static_cast<int64_t>(ts_ms_ll), null_cb);
                ++n;
            }
        }
        enabled = was_enabled;
        if (n == 0) omega::seed_die("XauDonchian55GatedM30", actual);
        printf("[SEED] XauDonchian55GatedM30: %zu M30 bars replayed from %s -- hot"
               " (ema_fast=%.2f ema_slow=%.2f atr=%.2f donchian_buf=%d)\n",
               n, actual.c_str(), ema_fast_, ema_slow_, atr_, (int)m30_highs_.size());
        fflush(stdout);
        return n;
    }
};

} // namespace omega
