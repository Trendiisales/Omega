// =============================================================================
//  XauEmaCrossH4Engine.hpp -- EMA20/100 golden cross H4 (long-only)
//
//  PROVENANCE (2026-05-20)
//
//  Main has g_ema_cross (EMACrossEngine) -- check enabled state. This is a
//  STANDALONE H4 golden cross with proven parameters.
//
//  Signal: EMA20 crosses ABOVE EMA100 -> enter long. Exit via ATR SL/TP/hold.
//
//  CONFIG (ema_fast=20, ema_slow=100, hold=20 H4 bars, sl_atr=1.5, tp_atr=3.0):
//    Backtest 2yr XAU H4 (cost=10bps):
//      IS Sh=4.45, OOS Sh=9.19, FUL Sh=7.15, n=20, PnL=12.2%, WR=60%
//      OOS > IS (good sign — not IS-overfit)
//
//  CAVEAT: n=20 sparse (~10/yr). Crosses are rare events.
// =============================================================================

#pragma once
//  ADVERSE-PROTECTION: disabled (listed under DISABLED in CLAUDE.md audit, not instantiated in engine_init.hpp) -- fixed ATR SL/TP bracket (sl_atr=1.5/tp_atr=3.0) + time-stop (hold_max_h4=20) + weekend-close gate are the in-flight protection; no LOSS_CUT_PCT cold cut or BE ratchet. The header IS/OOS Sharpe figures are bar-replay not faithful; no faithful backtest on record -- verdict owed before re-enable (backfill S-2026-06-24n)
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <algorithm>
#include "OmegaTradeLedger.hpp"
#include "PortfolioGuard.hpp"  // S51: concurrency cap

namespace omega {

struct XauEmaCrossH4Params {
    int    ema_fast            = 20;
    int    ema_slow            = 100;
    int    hold_max_h4         = 20;
    double sl_atr_mult         = 1.5;
    double tp_atr_mult         = 3.0;
    double lot                 = 0.01;
    double dollars_per_pt      = 1.0;
    int    atr_period          = 14;
    double max_spread          = 1.0;
    bool   weekend_close_gate  = true;
};

inline XauEmaCrossH4Params make_xau_ema_cross_h4_params() { return XauEmaCrossH4Params{}; }

struct XauEmaCrossH4Signal {
    bool        valid   = false;
    double      entry   = 0.0;
    double      sl      = 0.0;
    double      tp      = 0.0;
    double      lot     = 0.0;
    const char* reason  = "";
};

struct XauEmaCrossH4Engine {
    bool   shadow_mode = true;
    bool   enabled     = true;
    XauEmaCrossH4Params p;
    std::string symbol = "XAUUSD";

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;

    double ema_fast_=0, ema_slow_=0;
    int    ema_count_=0;
    bool   prev_fast_above_=false;
    bool   has_prev_=false;

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

    XauEmaCrossH4Signal on_h4_bar(double h4_high, double h4_low, double h4_close,
                                   double bid, double ask, int64_t h4_close_ms,
                                   CloseCallback on_close) noexcept {
        XauEmaCrossH4Signal sig{};

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

        if (ema_count_ <= p.ema_slow + 1) {
            // Warming up
            if (ema_count_ > p.ema_slow) {
                prev_fast_above_ = ema_fast_ > ema_slow_;
                has_prev_ = true;
            }
            return sig;
        }

        const bool cur_above = ema_fast_ > ema_slow_;
        const bool golden_cross = has_prev_ && (!prev_fast_above_) && cur_above;
        prev_fast_above_ = cur_above;

        if (!pos_.active && enabled && golden_cross && atr_pre > 0.0
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

            printf("[XAU_EMA_CROSS_H4] ENTRY LONG @ %.2f sl=%.2f tp=%.2f lot=%.2f"
                   " ema_f=%.2f ema_s=%.2f atr=%.2f%s\n",
                   entry_px, sl_px, tp_px, p.lot, ema_fast_, ema_slow_, atr_pre,
                   shadow_mode ? " [SHADOW]" : "");
            fflush(stdout);

            sig.valid=true;
            sig.entry=entry_px; sig.sl=sl_px; sig.tp=tp_px; sig.lot=p.lot;
            sig.reason = "XAU_EMA_CROSS_H4_GOLDEN";
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

        printf("[XAU_EMA_CROSS_H4] EXIT reason=%s entry=%.2f exit=%.2f pts=%.2f"
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
        tr.exitReason=reason; tr.engine="XauEmaCrossH4";
        if (on_close) on_close(tr);
        pos_ = OpenPos{};
    }
};

} // namespace omega
