// =============================================================================
//  EmaPullbackEngine.hpp -- Tier-3 ship: 4 ema_pullback long cells
//  (H1/H2/H4/H6 long) for live shadow on XAUUSD.
//
//  Created 2026-04-30. Verdict source:
//      phase1/signal_discovery/POST_CUT_FULL_REPORT.md
//      phase1/signal_discovery/post_cut_revalidate_all.py
//          (sig_ema_pullback + sim_a)
//
//  POST-CUT BACKTEST (post-2025-04 corpus, 1 unit, 365 days, sim_a, costs):
//      H1 long: 398 trades, 33.7% WR, +$  894, $+2.25/trade,  pf 1.22
//      H2 long: 212 trades, 38.2% WR, +$  962, $+4.54/trade,  pf 1.31
//      H4 long: 111 trades, 40.5% WR, +$  739, $+6.66/trade,  pf 1.34
//      H6 long:  75 trades, 48.0% WR, +$1,411, $+18.81/trade, pf 1.90
//      ----------
//      Combined: 796 trades, +$4,006/yr/unit
//
//  SIGNAL (mirrors phase1/signal_discovery/post_cut_revalidate_all.py::sig_ema_pullback):
//      ema_f = close.ewm(span=9, adjust=False)        # fast EMA
//      ema_s = close.ewm(span=21, adjust=False)       # slow EMA
//      slow_up   = ema_s > ema_s.shift(1)             # slow EMA rising
//      slow_down = ema_s < ema_s.shift(1)             # slow EMA falling
//      pull_lo = (low  <= ema_f) AND (close > ema_f)  # pulled back to fast, recovered
//      pull_hi = (high >= ema_f) AND (close < ema_f)
//      long  fires when slow_up   & pull_lo & (ema_f > ema_s)
//      short fires when slow_down & pull_hi & (ema_f < ema_s)
//      cooldown = 5 bars between fires (signal-side; mirrors Python)
//
//  Tier-3 here is LONG-ONLY -- the 4 profitable cells per the post-cut
//  report. Short variants were not in the master_summary profitable set.
//
//  EXIT (mirrors sim_a):
//      Entry at next bar open (live: ask for long at bar-close tick).
//      Hard SL = entry - sl_atr * ATR14_at_signal       (sl_atr=1.0)
//      Hard TP = entry + tp_r * sl_atr * ATR14_at_signal (tp_r=2.5)
//      max_hold = 30 bars; TIME_EXIT at bar close.
//      Intrabar SL fires before TP if both hit on same bar (matches sim_a).
//
//  EWM SEMANTICS:
//      pandas.ewm(span=N, adjust=False) uses alpha = 2/(N+1) and recursive
//      EMA: ema_t = alpha*x_t + (1-alpha)*ema_{t-1}, with ema_0 = x_0.
//      Reproduced here in EmaPullbackEMA::on_bar.
//
//  COOLDOWN: signal-fire cooldown (mirrors Python sig_ema_pullback). See the
//  DonchianEngine.hpp cooldown semantics block for the rationale.
//
//  WARMUP: reuses phase1/signal_discovery/tsmom_warmup_H1.csv (same H1 stream
//  input -- the EWM gets warm after ~50 H1 bars, so the post-cut CSV gives
//  every cell a fully warmed EWM + ATR + lookback before live ticks arrive).
// =============================================================================

#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include "OmegaTradeLedger.hpp"
#include "CellPrimitives.hpp"   // Phase 1 of CellEngine refactor; layout sanity asserts below

namespace omega {

struct EpbBar {
    int64_t bar_start_ms = 0;
    double  open  = 0.0;
    double  high  = 0.0;
    double  low   = 0.0;
    double  close = 0.0;
};

struct EpbBarSynth {
    int      stride       = 1;
    int      accum_count_ = 0;
    EpbBar   cur_{};
    using EmitCallback = std::function<void(const EpbBar&)>;

    void on_h1_bar(const EpbBar& h1, EmitCallback emit) noexcept {
        if (accum_count_ == 0) {
            cur_              = h1;
            cur_.bar_start_ms = h1.bar_start_ms;
            cur_.open         = h1.open;
        } else {
            cur_.high  = std::max(cur_.high, h1.high);
            cur_.low   = std::min(cur_.low,  h1.low);
            cur_.close = h1.close;
        }
        accum_count_++;
        if (accum_count_ >= stride) {
            if (emit) emit(cur_);
            accum_count_ = 0;
            cur_         = EpbBar{};
        }
    }
};

struct EpbATR14 {
    static constexpr int ATR_P = 14;
    bool   has_prev_close_ = false;
    double prev_close_     = 0.0;
    double atr_            = 0.0;
    int    n_              = 0;

    bool ready() const noexcept { return n_ >= ATR_P; }

    void on_bar(const EpbBar& b) noexcept {
        const double tr = has_prev_close_
            ? std::max({ b.high - b.low,
                         std::fabs(b.high - prev_close_),
                         std::fabs(b.low  - prev_close_) })
            : (b.high - b.low);
        if (n_ < ATR_P) { atr_ = (atr_ * n_ + tr) / (n_ + 1); n_++; }
        else            { atr_ = (atr_ * (ATR_P - 1) + tr) / ATR_P; }
        prev_close_     = b.close;
        has_prev_close_ = true;
    }

    double value() const noexcept { return atr_; }
};

// pandas.ewm(span=N, adjust=False) reproduced.
// alpha = 2/(N+1). On first observation, ema = x_0. After: ema_t = alpha*x_t + (1-alpha)*ema_{t-1}.
struct EpbEMA {
    int    span     = 9;
    double alpha    = 0.2;       // 2/(span+1) -- recomputed in init()
    bool   has_     = false;
    double value_   = 0.0;
    double prev_    = 0.0;       // ema value BEFORE the most recent on_bar

    void init(int s) noexcept {
        span  = s;
        alpha = 2.0 / (static_cast<double>(s) + 1.0);
        has_  = false;
        value_ = 0.0;
        prev_  = 0.0;
    }

    void on_close(double x) noexcept {
        prev_ = value_;
        if (!has_) { value_ = x; has_ = true; }
        else       { value_ = alpha * x + (1.0 - alpha) * value_; }
    }

    bool   has() const noexcept { return has_; }
    double value() const noexcept { return value_; }
    double prev()  const noexcept { return prev_; }
    bool   rising()  const noexcept { return has_ && value_ > prev_; }
    bool   falling() const noexcept { return has_ && value_ < prev_; }
};

// -----------------------------------------------------------------------------
//  Phase 1 structural-sanity gate (refactor plan §4 Phase 1).
//  These asserts confirm EpbBar/EpbBarSynth/EpbATR14/EpbEMA are layout-
//  compatible with the canonical omega::cell types declared in
//  CellPrimitives.hpp. Any drift breaks the V1/V2 shadow comparison that
//  Phase 2 depends on, so we want a hard compile-time stop here -- not a
//  runtime divergence later. Pure additive: no behaviour change.
// -----------------------------------------------------------------------------
static_assert(sizeof(EpbBar)      == sizeof(::omega::cell::Bar),
              "EpbBar size drift vs omega::cell::Bar");
static_assert(sizeof(EpbBarSynth) == sizeof(::omega::cell::BarSynth),
              "EpbBarSynth size drift vs omega::cell::BarSynth");
static_assert(sizeof(EpbATR14)    == sizeof(::omega::cell::ATR14),
              "EpbATR14 size drift vs omega::cell::ATR14");
static_assert(sizeof(EpbEMA)      == sizeof(::omega::cell::EMA),
              "EpbEMA size drift vs omega::cell::EMA");

// =============================================================================
//  EpbCell -- single ema_pullback cell (long-only by Tier-3 catalogue).
// =============================================================================
struct EpbCell {
    using OnCloseCb = std::function<void(const TradeRecord&)>;

    bool   shadow_mode          = true;
    bool   enabled              = true;
    int    fast_span            = 9;
    int    slow_span            = 21;
    int    max_hold_bars        = 30;
    int    signal_cooldown_bars = 5;
    double sl_atr               = 1.0;
    double tp_r                 = 2.5;
    double max_spread_pt        = 1.5;

    int    direction            = 1;
    std::string timeframe       = "H1";
    std::string symbol          = "XAUUSD";
    std::string cell_id         = "EmaPullback_H1_long";

    EpbEMA ema_f_;
    EpbEMA ema_s_;
    bool   has_first_bar_ = false;   // gate slow_up/down until 2nd bar onward

    // Position state
    bool    pos_active_     = false;
    double  pos_entry_      = 0.0;
    double  pos_sl_         = 0.0;
    double  pos_tp_         = 0.0;
    double  pos_size_       = 0.0;
    double  pos_atr_        = 0.0;
    int64_t pos_entry_ms_   = 0;
    int     pos_bars_held_  = 0;
    double  pos_mfe_        = 0.0;
    double  pos_mae_        = 0.0;
    double  pos_spread_at_  = 0.0;
    int     trade_id_       = 0;

    int    bar_count_           = 0;
    int    signal_cooldown_left_ = 0;

    bool has_open_position() const noexcept { return pos_active_; }

    void init_emas() noexcept {
        ema_f_.init(fast_span);
        ema_s_.init(slow_span);
        has_first_bar_ = false;
    }

    // -------------------------------------------------------------------------
    //  on_bar -- single completed parent bar.
    //  Returns 1 if a NEW position was opened this bar, 0 otherwise.
    //
    //  Order of operations (must match Python sim_a + sig_ema_pullback):
    //    1. Update fast + slow EMAs from this bar's close (pandas advances
    //       BEFORE evaluating the slow_up / slow_down comparison vs prev).
    //    2. Manage existing position (if any) using THIS bar's high/low for
    //       SL/TP intrabar checks.
    //    3. Decrement signal cooldown.
    //    4. Pre-fire gates.
    //    5. Compute signal: slow_up & pull_lo & (ema_f > ema_s) for long.
    //       Use this bar's high/low for the pull check.
    //    6. Open the position at next bar open (= ask at the bar-close tick).
    // -------------------------------------------------------------------------
    int on_bar(const EpbBar& b, double bid, double ask, double atr14_at_signal,
               int64_t now_ms, double size_lot, OnCloseCb on_close) noexcept {
        ++bar_count_;

        // 1. Update EMAs (slow_up/down requires prev value -- which on_close stamps)
        ema_f_.on_close(b.close);
        ema_s_.on_close(b.close);

        // 2. Manage existing position FIRST (uses bar's high/low for intrabar)
        if (pos_active_) {
            ++pos_bars_held_;
            const double max_move_this_bar = direction == 1
                ? (b.high - pos_entry_) : (pos_entry_ - b.low);
            const double min_move_this_bar = direction == 1
                ? (b.low  - pos_entry_) : (pos_entry_ - b.high);
            if (max_move_this_bar > pos_mfe_) pos_mfe_ = max_move_this_bar;
            if (min_move_this_bar < pos_mae_) pos_mae_ = min_move_this_bar;

            const bool sl_hit = direction == 1
                ? (b.low  <= pos_sl_) : (b.high >= pos_sl_);
            if (sl_hit) { _close(pos_sl_, "SL_HIT", now_ms, on_close); return 0; }
            const bool tp_hit = direction == 1
                ? (b.high >= pos_tp_) : (b.low  <= pos_tp_);
            if (tp_hit) { _close(pos_tp_, "TP_HIT", now_ms, on_close); return 0; }
            if (pos_bars_held_ >= max_hold_bars) {
                _close(b.close, "TIME_EXIT", now_ms, on_close); return 0;
            }
            return 0;
        }

        // 3. Decrement cooldown
        if (signal_cooldown_left_ > 0) --signal_cooldown_left_;

        // 4. Pre-fire gates
        if (!enabled)                                                  return 0;
        if (!ema_f_.has() || !ema_s_.has())                            return 0;
        // Need at least 2 bars to evaluate slow_up/down (prev != value).
        if (!has_first_bar_) { has_first_bar_ = true; return 0; }
        if (signal_cooldown_left_ > 0)                                 return 0;
        if (!std::isfinite(atr14_at_signal) || atr14_at_signal <= 0.0) return 0;
        const double spread_pt = ask - bid;
        if (!std::isfinite(spread_pt) || spread_pt < 0.0)              return 0;
        if (spread_pt > max_spread_pt)                                 return 0;
        if (size_lot <= 0.0)                                           return 0;

        // 5. Signal evaluation
        const double ef = ema_f_.value();
        const double es = ema_s_.value();
        const bool   slow_up   = ema_s_.rising();
        const bool   slow_down = ema_s_.falling();
        const bool   pull_lo = (b.low  <= ef) && (b.close > ef);
        const bool   pull_hi = (b.high >= ef) && (b.close < ef);

        const int sig_dir =
            (slow_up   && pull_lo && (ef > es)) ? +1 :
            (slow_down && pull_hi && (ef < es)) ? -1 : 0;
        if (sig_dir == 0) return 0;

        // ALWAYS arm cooldown when sig_dir != 0 (mirrors Python sig_ema_pullback).
        signal_cooldown_left_ = signal_cooldown_bars;

        if (sig_dir != direction) return 0;

        // 6. Open the position
        const double entry_px = direction == 1 ? ask : bid;
        const double sl_pts   = atr14_at_signal * sl_atr;
        const double tp_pts   = atr14_at_signal * tp_r * sl_atr;
        const double sl_px    = entry_px - direction * sl_pts;
        const double tp_px    = entry_px + direction * tp_pts;

        pos_active_     = true;
        pos_entry_      = entry_px;
        pos_sl_         = sl_px;
        pos_tp_         = tp_px;
        pos_size_       = size_lot;
        pos_atr_        = atr14_at_signal;
        pos_entry_ms_   = now_ms;
        pos_bars_held_  = 0;
        pos_mfe_        = 0.0;
        pos_mae_        = 0.0;
        pos_spread_at_  = spread_pt;
        ++trade_id_;

        printf("[%s] ENTRY %s @ %.2f sl=%.2f tp=%.2f size=%.4f atr=%.3f"
               " ef=%.2f es=%.2f close=%.2f spread=%.2f%s\n",
               cell_id.c_str(),
               direction == 1 ? "LONG" : "SHORT",
               entry_px, sl_px, tp_px, pos_size_, atr14_at_signal,
               ef, es, b.close, spread_pt,
               shadow_mode ? " [SHADOW]" : "");
        fflush(stdout);
        return 1;
    }

    void on_tick(double bid, double ask, int64_t now_ms,
                 OnCloseCb on_close) noexcept {
        if (!pos_active_) return;
        const double signed_move = direction == 1
            ? (bid - pos_entry_) : (pos_entry_ - ask);
        if (signed_move > pos_mfe_) pos_mfe_ = signed_move;
        if (signed_move < pos_mae_) pos_mae_ = signed_move;

        if (direction == 1) {
            if (bid <= pos_sl_) { _close(bid, "SL_HIT", now_ms, on_close); return; }
            if (bid >= pos_tp_) { _close(bid, "TP_HIT", now_ms, on_close); return; }
        } else {
            if (ask >= pos_sl_) { _close(ask, "SL_HIT", now_ms, on_close); return; }
            if (ask <= pos_tp_) { _close(ask, "TP_HIT", now_ms, on_close); return; }
        }
    }

    void force_close(double bid, double ask, int64_t now_ms,
                     OnCloseCb on_close) noexcept {
        if (!pos_active_) return;
        const double exit_px = direction == 1 ? bid : ask;
        _close(exit_px, "FORCE_CLOSE", now_ms, on_close);
    }

private:
    void _close(double exit_px, const char* reason, int64_t now_ms,
                OnCloseCb on_close) noexcept {
        const double pnl_pts = (exit_px - pos_entry_) * direction * pos_size_;
        printf("[%s] EXIT %s @ %.2f %s pnl=$%.2f bars=%d mfe=%.1fpt mae=%.1fpt%s\n",
               cell_id.c_str(),
               direction == 1 ? "LONG" : "SHORT",
               exit_px, reason, pnl_pts * 100.0, pos_bars_held_,
               pos_mfe_, pos_mae_,
               shadow_mode ? " [SHADOW]" : "");
        fflush(stdout);

        if (on_close) {
            TradeRecord tr;
            tr.id            = trade_id_;
            tr.symbol        = symbol;
            tr.side          = direction == 1 ? "LONG" : "SHORT";
            tr.engine        = cell_id;
            tr.entryPrice    = pos_entry_;
            tr.exitPrice     = exit_px;
            tr.sl            = pos_sl_;
            tr.tp            = pos_tp_;
            tr.size          = pos_size_;
            tr.pnl           = pnl_pts;
            tr.mfe           = pos_mfe_ * pos_size_;
            tr.mae           = pos_mae_ * pos_size_;
            tr.entryTs       = pos_entry_ms_ / 1000;
            tr.exitTs        = now_ms / 1000;
            tr.exitReason    = reason;
            tr.regime        = "EMA_PULLBACK";
            tr.atr_at_entry  = pos_atr_;
            tr.spreadAtEntry = pos_spread_at_;
            tr.shadow        = shadow_mode;
            on_close(tr);
        }

        pos_active_     = false;
        pos_entry_      = 0.0;
        pos_sl_         = 0.0;
        pos_tp_         = 0.0;
        pos_size_       = 0.0;
        pos_atr_        = 0.0;
        pos_entry_ms_   = 0;
        pos_bars_held_  = 0;
        pos_mfe_        = 0.0;
        pos_mae_        = 0.0;
        pos_spread_at_  = 0.0;
    }
};

// =============================================================================
//  EpbPortfolio -- 4 long ema_pullback cells (H1/H2/H4/H6).
// =============================================================================
struct EpbPortfolio {
    using OnCloseCb = std::function<void(const TradeRecord&)>;

    bool   enabled           = true;
    bool   shadow_mode       = true;
    int    max_concurrent    = 4;
    double risk_pct          = 0.005;
    double start_equity      = 10000.0;
    double margin_call       = 1000.0;
    double max_lot_cap       = 0.05;
    bool   block_on_risk_off = true;
    std::string warmup_csv_path = "";

    EpbCell h1_long_;
    EpbCell h2_long_;
    EpbCell h4_long_;
    EpbCell h6_long_;

    // H1 cell does not need a synth (uses raw H1 input).
    EpbBarSynth synth_h2_;     // stride 2
    EpbBarSynth synth_h4_;     // stride 4
    EpbBarSynth synth_h6_;     // stride 6

    EpbATR14 atr_h1_;
    EpbATR14 atr_h2_;
    EpbATR14 atr_h4_;
    EpbATR14 atr_h6_;

    double equity_                 = 10000.0;
    double peak_equity_            = 10000.0;
    double max_dd_pct_             = 0.0;
    int    open_count_             = 0;
    int    blocked_max_concurrent_ = 0;
    int    blocked_risk_off_       = 0;

    std::string macro_regime_      = "NEUTRAL";
    void set_macro_regime(const std::string& r) noexcept { macro_regime_ = r; }

    void init() noexcept {
        auto stamp = [](EpbCell& c, const char* tf, const char* id) {
            c.symbol     = "XAUUSD";
            c.cell_id    = id;
            c.timeframe  = tf;
            c.direction  = +1;        // Tier-3 long-only
            c.fast_span  = 9;
            c.slow_span  = 21;
            c.sl_atr     = 1.0;
            c.tp_r       = 2.5;
            c.max_hold_bars = 30;
            c.signal_cooldown_bars = 5;
            c.init_emas();
        };
        stamp(h1_long_, "H1", "EmaPullback_H1_long");
        stamp(h2_long_, "H2", "EmaPullback_H2_long");
        stamp(h4_long_, "H4", "EmaPullback_H4_long");
        stamp(h6_long_, "H6", "EmaPullback_H6_long");

        EpbCell* cells[] = { &h1_long_, &h2_long_, &h4_long_, &h6_long_ };
        for (EpbCell* c : cells) c->shadow_mode = shadow_mode;

        synth_h2_.stride = 2;
        synth_h4_.stride = 4;
        synth_h6_.stride = 6;

        equity_      = start_equity;
        peak_equity_ = start_equity;

        printf("[EPB] EmaPullbackPortfolio ARMED (shadow_mode=%s, enabled=%s) "
               "cells=H1,H2,H4,H6 long fast=9 slow=21 sl_atr=1.0 tp_r=2.5 "
               "max_hold=30 cooldown=5 risk_pct=%.4f max_lot_cap=%.3f "
               "max_concurrent=%d block_on_risk_off=%s equity=$%.2f\n",
               shadow_mode ? "true" : "false",
               enabled     ? "true" : "false",
               risk_pct, max_lot_cap, max_concurrent,
               block_on_risk_off ? "true" : "false",
               equity_);
        for (EpbCell* c : cells) {
            printf("[%s] ARMED (shadow_mode=%s, fast=%d, slow=%d, sl=%.1f*atr, tp=%.2f*atr, max_hold=%d)\n",
                   c->cell_id.c_str(),
                   c->shadow_mode ? "true" : "false",
                   c->fast_span, c->slow_span,
                   c->sl_atr, c->sl_atr * c->tp_r, c->max_hold_bars);
        }
        fflush(stdout);
    }

    double size_for(double sl_pts) const noexcept {
        if (sl_pts <= 0.0) return 0.0;
        const double risk_target  = equity_ * risk_pct;
        const double risk_per_lot = sl_pts * 100.0;
        if (risk_per_lot <= 0.0) return 0.0;
        double lot = risk_target / risk_per_lot;
        lot = std::floor(lot / 0.01) * 0.01;
        return std::max(0.01, std::min(max_lot_cap, lot));
    }

    bool can_open() const noexcept {
        if (!enabled)                 return false;
        if (equity_ < margin_call)    return false;
        if (block_on_risk_off && macro_regime_ == "RISK_OFF") return false;
        return open_count_ < max_concurrent;
    }

    OnCloseCb wrap(OnCloseCb runtime_cb, int /*cell_idx*/) {
        return [this, runtime_cb](const TradeRecord& tr) {
            const double pnl_usd = tr.pnl * 100.0;
            equity_ += pnl_usd;
            if (equity_ > peak_equity_) peak_equity_ = equity_;
            const double dd = peak_equity_ > 0.0
                ? (equity_ - peak_equity_) / peak_equity_ : 0.0;
            if (dd < max_dd_pct_) max_dd_pct_ = dd;
            open_count_ = std::max(0, open_count_ - 1);
            printf("[EPB-CLOSE] cell=%s pnl=$%.2f equity=$%.2f peak=$%.2f"
                   " dd=%.2f%% open=%d\n",
                   tr.engine.c_str(), pnl_usd, equity_, peak_equity_,
                   max_dd_pct_ * 100.0, open_count_);
            fflush(stdout);
            if (runtime_cb) runtime_cb(tr);
        };
    }

    void _drive_cell(EpbCell& cell, const EpbBar& b, double bid, double ask,
                     double atr14, int cell_idx, int64_t now_ms,
                     OnCloseCb runtime_cb) noexcept {
        if (cell.has_open_position()) {
            (void)cell.on_bar(b, bid, ask, atr14, now_ms, 0.0, wrap(runtime_cb, cell_idx));
            return;
        }
        if (!can_open()) {
            if (block_on_risk_off && macro_regime_ == "RISK_OFF") ++blocked_risk_off_;
            else                                                  ++blocked_max_concurrent_;
            (void)cell.on_bar(b, bid, ask, atr14, now_ms, 0.0, wrap(runtime_cb, cell_idx));
            return;
        }
        const double sl_pts = atr14 * cell.sl_atr;
        const double lot    = size_for(sl_pts);
        const int    opened = cell.on_bar(b, bid, ask, atr14, now_ms, lot, wrap(runtime_cb, cell_idx));
        if (opened) ++open_count_;
    }

    // -------------------------------------------------------------------------
    //  on_h1_bar -- single integration point. H1 cell uses raw H1 bars and
    //  the runtime-supplied ATR (with internal fallback when cold);
    //  H2/H4/H6 cells use synthesised bars + self-computed ATR.
    // -------------------------------------------------------------------------
    void on_h1_bar(const EpbBar& h1, double bid, double ask, double h1_atr14,
                   int64_t now_ms, OnCloseCb runtime_cb) noexcept {
        if (!enabled) return;

        atr_h1_.on_bar(h1);
        const double eff_h1_atr =
            (std::isfinite(h1_atr14) && h1_atr14 > 0.0) ? h1_atr14 : atr_h1_.value();
        if (eff_h1_atr > 0.0) {
            _drive_cell(h1_long_, h1, bid, ask, eff_h1_atr, 0, now_ms, runtime_cb);
        } else {
            (void)h1_long_.on_bar(h1, bid, ask, 0.0, now_ms, 0.0, wrap(runtime_cb, 0));
        }

        synth_h2_.on_h1_bar(h1, [&](const EpbBar& b) {
            atr_h2_.on_bar(b);
            if (atr_h2_.ready()) _drive_cell(h2_long_, b, bid, ask, atr_h2_.value(), 1, now_ms, runtime_cb);
            else                 (void)h2_long_.on_bar(b, bid, ask, 0.0, now_ms, 0.0, wrap(runtime_cb, 1));
        });
        synth_h4_.on_h1_bar(h1, [&](const EpbBar& b) {
            atr_h4_.on_bar(b);
            if (atr_h4_.ready()) _drive_cell(h4_long_, b, bid, ask, atr_h4_.value(), 2, now_ms, runtime_cb);
            else                 (void)h4_long_.on_bar(b, bid, ask, 0.0, now_ms, 0.0, wrap(runtime_cb, 2));
        });
        synth_h6_.on_h1_bar(h1, [&](const EpbBar& b) {
            atr_h6_.on_bar(b);
            if (atr_h6_.ready()) _drive_cell(h6_long_, b, bid, ask, atr_h6_.value(), 3, now_ms, runtime_cb);
            else                 (void)h6_long_.on_bar(b, bid, ask, 0.0, now_ms, 0.0, wrap(runtime_cb, 3));
        });
    }

    void on_tick(double bid, double ask, int64_t now_ms,
                 OnCloseCb runtime_cb) noexcept {
        if (!enabled) return;
        h1_long_.on_tick(bid, ask, now_ms, wrap(runtime_cb, 0));
        h2_long_.on_tick(bid, ask, now_ms, wrap(runtime_cb, 1));
        h4_long_.on_tick(bid, ask, now_ms, wrap(runtime_cb, 2));
        h6_long_.on_tick(bid, ask, now_ms, wrap(runtime_cb, 3));
    }

    void force_close_all(double bid, double ask, int64_t now_ms,
                         OnCloseCb runtime_cb) noexcept {
        h1_long_.force_close(bid, ask, now_ms, wrap(runtime_cb, 0));
        h2_long_.force_close(bid, ask, now_ms, wrap(runtime_cb, 1));
        h4_long_.force_close(bid, ask, now_ms, wrap(runtime_cb, 2));
        h6_long_.force_close(bid, ask, now_ms, wrap(runtime_cb, 3));
    }

    int total_open() const noexcept {
        return (h1_long_.has_open_position() ? 1 : 0)
             + (h2_long_.has_open_position() ? 1 : 0)
             + (h4_long_.has_open_position() ? 1 : 0)
             + (h6_long_.has_open_position() ? 1 : 0);
    }

    int warmup_from_csv(const std::string& path) noexcept {
        if (!enabled) {
            printf("[EPB-WARMUP] skipped -- portfolio disabled\n"); fflush(stdout); return 0;
        }
        if (path.empty()) {
            printf("[EPB-WARMUP] skipped -- warmup_csv_path empty (cold start)\n"); fflush(stdout); return 0;
        }
        std::ifstream f(path);
        if (!f.is_open()) {
            printf("[EPB-WARMUP] FAIL -- cannot open '%s'\n", path.c_str()); fflush(stdout); return 0;
        }

        int     fed = 0, rejected = 0;
        int64_t first_ms = 0, last_ms = 0;
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;

            std::array<std::string, 5> tok;
            int idx = 0;
            std::stringstream ss(line);
            std::string field;
            while (idx < 5 && std::getline(ss, field, ',')) tok[idx++] = field;
            if (idx < 5) { ++rejected; continue; }

            char* endp = nullptr;
            const long long ms_ll = std::strtoll(tok[0].c_str(), &endp, 10);
            if (endp == tok[0].c_str() || *endp != '\0') { ++rejected; continue; }

            char* ep_o = nullptr; const double o = std::strtod(tok[1].c_str(), &ep_o);
            char* ep_h = nullptr; const double h = std::strtod(tok[2].c_str(), &ep_h);
            char* ep_l = nullptr; const double l = std::strtod(tok[3].c_str(), &ep_l);
            char* ep_c = nullptr; const double c = std::strtod(tok[4].c_str(), &ep_c);
            if (ep_o == tok[1].c_str() || ep_h == tok[2].c_str()
                || ep_l == tok[3].c_str() || ep_c == tok[4].c_str()) { ++rejected; continue; }
            if (!std::isfinite(o) || !std::isfinite(h)
                || !std::isfinite(l) || !std::isfinite(c))   { ++rejected; continue; }

            EpbBar b;
            b.bar_start_ms = static_cast<int64_t>(ms_ll);
            b.open = o; b.high = h; b.low = l; b.close = c;
            if (fed == 0) first_ms = b.bar_start_ms;
            last_ms = b.bar_start_ms;
            _feed_warmup_h1_bar(b);
            ++fed;
        }

        printf("[EPB-WARMUP] fed=%d rejected=%d first_ms=%lld last_ms=%lld path='%s'\n",
               fed, rejected,
               static_cast<long long>(first_ms),
               static_cast<long long>(last_ms),
               path.c_str());
        printf("[EPB-WARMUP] cell readiness: "
               "H1 ema_f=%s ema_s=%s atr=%s  "
               "H2 ema_f=%s atr=%s  H4 ema_f=%s atr=%s  H6 ema_f=%s atr=%s\n",
               h1_long_.ema_f_.has() ? "ready" : "cold",
               h1_long_.ema_s_.has() ? "ready" : "cold",
               atr_h1_.ready() ? "ready" : "cold",
               h2_long_.ema_f_.has() ? "ready" : "cold",
               atr_h2_.ready() ? "ready" : "cold",
               h4_long_.ema_f_.has() ? "ready" : "cold",
               atr_h4_.ready() ? "ready" : "cold",
               h6_long_.ema_f_.has() ? "ready" : "cold",
               atr_h6_.ready() ? "ready" : "cold");
        fflush(stdout);
        return fed;
    }

private:
    void _feed_warmup_h1_bar(const EpbBar& h1) noexcept {
        auto noop_cb = OnCloseCb{};
        const double bid    = h1.close;
        const double ask    = h1.close;
        const int64_t now_ms = h1.bar_start_ms;

        atr_h1_.on_bar(h1);
        (void)h1_long_.on_bar(h1, bid, ask, atr_h1_.value(), now_ms, 0.0, noop_cb);

        synth_h2_.on_h1_bar(h1, [&](const EpbBar& b) {
            atr_h2_.on_bar(b);
            (void)h2_long_.on_bar(b, bid, ask, atr_h2_.value(), now_ms, 0.0, noop_cb);
        });
        synth_h4_.on_h1_bar(h1, [&](const EpbBar& b) {
            atr_h4_.on_bar(b);
            (void)h4_long_.on_bar(b, bid, ask, atr_h4_.value(), now_ms, 0.0, noop_cb);
        });
        synth_h6_.on_h1_bar(h1, [&](const EpbBar& b) {
            atr_h6_.on_bar(b);
            (void)h6_long_.on_bar(b, bid, ask, atr_h6_.value(), now_ms, 0.0, noop_cb);
        });
    }
};

} // namespace omega
