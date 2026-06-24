// =============================================================================
//  NasBbRevLongH1Engine.hpp -- NAS100 H1 Bollinger-Band reversion LONG + trail
//
//  PROVENANCE (2026-05-24)
//
//  /Users/jo/edge_research research. NAS100 H1, mean-reversion: enter LONG
//  when close < lower BB(20, 2σ) AND RSI(14) < 30. BE-then-trail at 1.5×ATR.
//
//  Validation:
//    27-month backtest 2024-01 → 2026-04:
//      Walk-forward 4 folds: all positive, OOS aggregate +$7912 / 145 trades
//      Best single-cell: sl=2.0 ATR tp=3R mb=96
//    No NAS100 L2 data available yet — L2 validation pending.
//
//  SIGNAL
//    LONG: close[0] < lower_bb AND rsi[0] < 30 AND rsi[-1] >= 30 (cross-down arm)
//          (i.e. trigger on RSI cross into oversold)
//
//  EXIT
//    Hard SL: 2.0 × ATR(14) from entry
//    Hard TP: 3R
//    Time:   96 H1 bars (4d)
//    Trail:  BE-then-trail: once MFE >= 1R, move SL to BE.
//            Once MFE >= 2R, switch SL to extreme - 1.5×ATR (trailing).
// =============================================================================

#pragma once
//  ADVERSE-PROTECTION: tombstoned (TOMBSTONES.tsv 2026-06-10 g_nas_bbrev_long_h1, UNVERIFIED/FAITHFUL-BT-REQUIRED) + disabled (enabled=false, engine_init.hpp:1752 S47 scalp/mean-rev purge); has a fixed 2.0xATR SL / 3R TP bracket + BE-arm@1R then 1.5xATR trail + 96-bar TIMEOUT, but no faithful backtest on record -- verdict owed before re-enable (backfill S-2026-06-24n)
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

struct NasBbRevLongH1Params {
    int    bb_period       = 20;
    double bb_k            = 2.0;
    int    rsi_period      = 14;
    double rsi_oversold    = 30.0;
    int    atr_period      = 14;
    double sl_atr_mult     = 2.0;
    double tp_r_mult       = 3.0;
    int    hold_max_bars   = 96;
    double trail_be_arm_R  = 1.0;
    double trail_switch_R  = 2.0;
    double trail_atr_mult  = 1.5;
    double lot             = 0.01;
    double max_spread      = 5.0;   // NAS100 typical spread larger
    int    cooldown_bars   = 1;
};

struct NasBbRevLongH1Signal {
    bool        valid  = false;
    double      entry  = 0.0;
    double      sl     = 0.0;
    double      tp     = 0.0;
    double      lot    = 0.0;
    const char* reason = "";
};

struct NasBbRevLongH1Engine {
    bool shadow_mode = true;
    bool enabled     = false;
    NasBbRevLongH1Params p;
    std::string symbol = "NAS100";

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;

    std::deque<double> closes_;
    std::deque<double> gains_, losses_;
    double atr_ = 0.0;
    int    atr_seed_count_ = 0;
    double atr_seed_sum_   = 0.0;
    double prev_close_     = 0.0;
    double prev_rsi_       = 50.0;
    int    bar_count_      = 0;
    int    cooldown_       = 0;

    struct OpenPos {
        bool active = false;
        double entry = 0, sl = 0, tp = 0, sl_dist = 0, lot = 0;
        double mfe = 0, extreme = 0;
        bool be_armed = false;     // moved to BE
        bool trail_switched = false; // switched to ATR-trail
        int64_t entry_ts_ms = 0;
        int bars_held = 0;
    } pos_;

    int m_trade_id_ = 0;
    bool has_open_position() const noexcept { return pos_.active; }

    void _close(double exit_px, const char* reason, int64_t now_ms,
                CloseCallback cb) noexcept {
        if (!pos_.active) return;
        const double pnl = (exit_px - pos_.entry) * pos_.lot;
        omega::TradeRecord tr{};
        tr.symbol     = symbol;
        tr.side       = "LONG";
        tr.engine     = "NasBbRevLongH1";
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
        printf("[NAS_BB_REV_H1] CLOSE LONG @ %.2f entry=%.2f pnl=%.2f"
               " mfe=%.2f reason=%s bars=%d%s\n",
               exit_px, pos_.entry, pnl, pos_.mfe, reason, pos_.bars_held,
               shadow_mode ? " [SHADOW]" : "");
        fflush(stdout);
        pos_ = OpenPos{};
        cooldown_ = p.cooldown_bars;
    }

    double _rsi() const noexcept {
        if ((int)gains_.size() < p.rsi_period) return 50.0;
        double avg_g = 0, avg_l = 0;
        for (auto g : gains_)  avg_g += g;
        for (auto l : losses_) avg_l += l;
        avg_g /= p.rsi_period; avg_l /= p.rsi_period;
        if (avg_l <= 0.0) return 100.0;
        const double rs = avg_g / avg_l;
        return 100.0 - (100.0 / (1.0 + rs));
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
        // RSI update (gain/loss of close-to-close)
        const double d = close - prev_close_;
        gains_.push_back (d > 0 ?  d : 0.0);
        losses_.push_back(d < 0 ? -d : 0.0);
        while ((int)gains_.size()  > p.rsi_period) gains_.pop_front();
        while ((int)losses_.size() > p.rsi_period) losses_.pop_front();
        prev_close_ = close;
    }

    void on_tick(double bid, double ask, int64_t now_ms,
                 CloseCallback cb) noexcept {
        if (!pos_.active || bid <= 0 || ask <= 0) return;
        const double mid = (bid + ask) * 0.5;
        const double move = mid - pos_.entry;       // long-only
        if (move > pos_.mfe) pos_.mfe = move;
        if (mid > pos_.extreme) pos_.extreme = mid;

        if (pos_.sl_dist > 0.0) {
            const double mfe_R = move / pos_.sl_dist;
            // arm BE
            if (!pos_.be_armed && mfe_R >= p.trail_be_arm_R) {
                pos_.be_armed = true;
                if (pos_.entry > pos_.sl) pos_.sl = pos_.entry;
            }
            // switch to ATR-trail
            if (!pos_.trail_switched && mfe_R >= p.trail_switch_R) {
                pos_.trail_switched = true;
            }
            // update trail SL once switched
            if (pos_.trail_switched && atr_ > 0.0) {
                const double trail_sl = pos_.extreme - p.trail_atr_mult * atr_;
                if (trail_sl > pos_.sl) pos_.sl = trail_sl;
            }
        }
        if (bid <= pos_.sl) {
            const char* r = pos_.trail_switched ? "TRAIL_SL"
                          : (pos_.be_armed     ? "BE_HIT" : "SL_HIT");
            _close(bid, r, now_ms, cb);
        } else if (bid >= pos_.tp) {
            _close(bid, "TP_HIT", now_ms, cb);
        }
    }

    NasBbRevLongH1Signal on_h1_bar(double h1_high, double h1_low, double h1_close,
                                    double bid, double ask, int64_t bar_close_ms,
                                    CloseCallback cb) noexcept {
        NasBbRevLongH1Signal sig{};

        closes_.push_back(h1_close);
        const int keep = std::max(p.bb_period, p.rsi_period) + 2;
        while ((int)closes_.size() > keep) closes_.pop_front();
        _update_atr(h1_high, h1_low, h1_close);
        ++bar_count_;
        if (cooldown_ > 0) --cooldown_;

        if (pos_.active) {
            ++pos_.bars_held;
            if (pos_.bars_held >= p.hold_max_bars)
                _close((bid+ask)*0.5, "TIMEOUT", bar_close_ms, cb);
        }

        const double cur_rsi = _rsi();
        const double rsi_prev_local = prev_rsi_;
        prev_rsi_ = cur_rsi;

        if (!enabled || pos_.active || cooldown_ > 0) return sig;
        if (atr_ <= 0.0) return sig;
        if ((ask - bid) > p.max_spread) return sig;
        if ((int)closes_.size() < p.bb_period) return sig;

        // Bollinger bands (close-based, last bb_period closes)
        double sum = 0, sum_sq = 0;
        const int sz = (int)closes_.size();
        for (int i = sz - p.bb_period; i < sz; ++i) {
            sum    += closes_[i];
            sum_sq += closes_[i] * closes_[i];
        }
        const double mean = sum / p.bb_period;
        const double var  = (sum_sq / p.bb_period) - (mean * mean);
        const double std_  = var > 0.0 ? std::sqrt(var) : 0.0;
        const double lower = mean - p.bb_k * std_;

        // signal: close below lower band AND rsi crossed into oversold
        const bool bb_trigger  = h1_close < lower;
        const bool rsi_trigger = (cur_rsi < p.rsi_oversold) &&
                                 (rsi_prev_local >= p.rsi_oversold);
        if (!(bb_trigger && rsi_trigger)) return sig;

        const double entry   = ask;
        const double sl_dist = p.sl_atr_mult * atr_;
        const double sl      = entry - sl_dist;
        const double tp      = entry + p.tp_r_mult * sl_dist;

        pos_.active = true;
        pos_.entry = entry; pos_.sl = sl; pos_.tp = tp; pos_.sl_dist = sl_dist;
        pos_.lot = p.lot; pos_.mfe = 0; pos_.extreme = entry;
        pos_.be_armed = false; pos_.trail_switched = false;
        pos_.entry_ts_ms = bar_close_ms; pos_.bars_held = 0;
        ++m_trade_id_;

        printf("[NAS_BB_REV_H1] ENTRY LONG @ %.2f sl=%.2f tp=%.2f atr=%.2f"
               " lower_bb=%.2f rsi=%.1f%s\n",
               entry, sl, tp, atr_, lower, cur_rsi,
               shadow_mode ? " [SHADOW]" : "");
        fflush(stdout);

        sig.valid = true; sig.entry = entry; sig.sl = sl; sig.tp = tp; sig.lot = p.lot;
        sig.reason = "NAS_BB_REV_H1_LONG";
        return sig;
    }

    // Warm-seed from H1 bar CSV. Format: ts,o,h,l,c (ts in SECONDS).
    size_t seed_from_h1_csv(const std::string& path) noexcept {
        const std::string actual = omega::resolve_seed_path(path);
        std::ifstream f(actual);
        if (!f.is_open()) omega::seed_die("NasBbRevLongH1", actual);
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
                const double sp = 1.5;   // synthetic NAS100 spread
                const double bid = c - sp/2, ask = c + sp/2;
                on_h1_bar(h, l, c, bid, ask, static_cast<int64_t>(ts_s_ll)*1000LL, null_cb);
                ++n;
            }
        }
        enabled = was_enabled;
        if (n == 0) omega::seed_die("NasBbRevLongH1", actual);
        printf("[SEED] NasBbRevLongH1: %zu H1 bars replayed from %s -- hot"
               " (atr=%.2f closes_buf=%d)\n",
               n, actual.c_str(), atr_, (int)closes_.size());
        fflush(stdout);
        return n;
    }
};

} // namespace omega
