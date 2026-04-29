// =============================================================================
//  DonchianEngine.hpp -- Tier-2 ship: 7 donchian cells (H2 long, H4/H6/D1
//  long+short) for live shadow on XAUUSD.
//
//  Created 2026-04-30. Verdict source:
//      phase1/signal_discovery/POST_CUT_FULL_REPORT.md
//      phase1/signal_discovery/post_cut_revalidate_all.py
//          (sig_donchian + sim_a)
//
//  IMPORTANT: donchian H1 long is NOT in this engine. The retuned H1 long
//  cell (sl_atr=3.0, tp_r=1.667, max_hold=30) is already live via
//  C1RetunedPortfolio.hpp (Phase 2 winner). Shipping H1 long here would
//  duplicate that fire path. The 7 cells in this engine are the bidirectional
//  H4/H6/D1 + the H2 long that C1Retuned doesn't cover.
//
//  POST-CUT BACKTEST (post-2025-04 corpus, 1 unit, 365 days, sim_a, costs):
//      H2 long:   127 trades, 40.2% WR, +$  692, $+5.45/trade,  pf 1.40
//      H4 long:    79 trades, 43.0% WR, +$1,168, $+14.78/trade, pf 1.93
//      H4 short:   29 trades, 41.4% WR, +$  946, $+32.62/trade, pf 2.84
//      H6 long:    52 trades, 53.8% WR, +$1,596, $+30.69/trade, pf 2.94
//      H6 short:   22 trades, 27.3% WR, +$  129, $+5.87/trade,  pf 1.19
//      D1 long:    16 trades, 56.2% WR, +$  862, $+53.86/trade, pf 3.17
//      D1 short:    3 trades, 33.3% WR, +$  227, $+75.81/trade, pf 3.00
//      ----------
//      Combined:  328 trades                    +$5,620 = 47% of unshipped edge
//
//  Bidirectional design: would have profited during the 2026-03-18 BEAR
//  cluster that the long-only C1Retuned cells lost on 4-of-4.
//
//  SIGNAL (mirrors phase1/signal_discovery/post_cut_revalidate_all.py::sig_donchian):
//      prior_high = rolling(period).max().shift(1)    # period=20
//      prior_low  = rolling(period).min().shift(1)
//      long  fires when close > prior_high
//      short fires when close < prior_low
//      cooldown = 5 bars between any fires (signal-side; not post-trade)
//
//  EXIT (mirrors sim_a):
//      Entry at next bar open (live runtime: ask for long, bid for short at
//      bar-close tick).
//      Hard SL = entry +/- sl_atr * ATR14_at_signal
//      Hard TP = entry +/- tp_r * sl_atr * ATR14_at_signal
//      max_hold = 30 bars; TIME_EXIT at bar close after max_hold elapses.
//      Intrabar SL fills first if both SL+TP hit on same bar (matches sim_a).
//
//  COOLDOWN SEMANTICS (different from TsmomEngine):
//  TsmomCell uses post-trade cooldown (counts from position close). The
//  Python sig_donchian uses signal-fire cooldown (counts from last signal
//  evaluation that produced a non-zero). DonchianCell mirrors the Python:
//  signal_cooldown_left_ ticks down on every bar regardless of position
//  state, and is set to signal_cooldown_bars when a signal fires (whether
//  or not it produced a position open). For the typical case where every
//  fire opens a position, this is functionally equivalent to a post-fire
//  cooldown of 5 bars; for windows where a position is already open and
//  the signal is suppressed (single-position assumption), the cooldown
//  still decrements so the next signal-allowed bar is reached on schedule.
//
//  SINGLE-POSITION ASSUMPTION (deviation from sim_a):
//  sim_a opens a NEW position for every non-zero signal -- positions can
//  overlap. Live runtime enforces single-position per cell (mirrors
//  TsmomCell / C1Retuned cells). For donchian's actual fire rates
//  (16-127 trades/yr per cell), overlapping fires are rare; the deviation
//  should be small. If first-week shadow shows trade count materially
//  below backtest expectations, revisit with multi-position support.
//
//  USAGE
//  -----
//      // globals.hpp:
//      static omega::DonchianPortfolio g_donchian;
//
//      // engine_init.hpp (after g_tsmom init):
//      g_donchian.shadow_mode       = kShadowDefault;
//      g_donchian.enabled           = true;
//      g_donchian.max_concurrent    = 7;
//      g_donchian.risk_pct          = 0.005;
//      g_donchian.start_equity      = 10000.0;
//      g_donchian.margin_call       = 1000.0;
//      g_donchian.max_lot_cap       = 0.05;
//      g_donchian.block_on_risk_off = true;
//      g_donchian.warmup_csv_path   = "phase1/signal_discovery/tsmom_warmup_H1.csv";
//      g_donchian.init();
//      g_donchian.warmup_from_csv(g_donchian.warmup_csv_path);
//
//      // tick_gold.hpp inside H1-close branch (after g_tsmom.on_h1_bar):
//      omega::DonchianBar dn_h1{};
//      dn_h1.bar_start_ms = s_bar_h1_ms;
//      dn_h1.open  = s_cur_h1.open;
//      dn_h1.high  = s_cur_h1.high;
//      dn_h1.low   = s_cur_h1.low;
//      dn_h1.close = s_cur_h1.close;
//      g_donchian.set_macro_regime(g_macroDetector.regime());
//      g_donchian.on_h1_bar(dn_h1, bid, ask,
//          g_bars_gold.h1.ind.atr14.load(std::memory_order_relaxed),
//          now_ms_g, ca_on_close);
//
//      // tick_gold.hpp on every tick (alongside g_tsmom.on_tick):
//      g_donchian.on_tick(bid, ask, now_ms_g, ca_on_close);
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

namespace omega {

// ---- Bar struct + synthesiser + ATR (parallel to TsmomEngine helpers; kept
//      as separate types so the two engines compile independently and a
//      future change to one doesn't accidentally affect the other). ----------
struct DonchianBar {
    int64_t bar_start_ms = 0;
    double  open  = 0.0;
    double  high  = 0.0;
    double  low   = 0.0;
    double  close = 0.0;
};

struct DonchianBarSynth {
    int          stride       = 1;
    int          accum_count_ = 0;
    DonchianBar  cur_{};
    using EmitCallback = std::function<void(const DonchianBar&)>;

    void on_h1_bar(const DonchianBar& h1, EmitCallback emit) noexcept {
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
            cur_         = DonchianBar{};
        }
    }
};

struct DonchianATR14 {
    static constexpr int ATR_P = 14;
    bool   has_prev_close_ = false;
    double prev_close_     = 0.0;
    double atr_            = 0.0;
    int    n_              = 0;

    bool ready() const noexcept { return n_ >= ATR_P; }

    void on_bar(const DonchianBar& b) noexcept {
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

// =============================================================================
//  DonchianCell -- single donchian cell.
//  Bar-driven: on_bar() runs at every parent-TF close. on_tick() runs every
//  XAUUSD tick (fires intrabar SL/TP between bars). force_close() is the
//  shutdown path.
// =============================================================================
struct DonchianCell {
    using OnCloseCb = std::function<void(const TradeRecord&)>;

    // ---- Configuration (set at init; immutable during run) ------------------
    bool   shadow_mode          = true;
    bool   enabled              = true;
    int    period               = 20;        // breakout window
    int    max_hold_bars        = 30;
    int    signal_cooldown_bars = 5;         // mirrors Python sig_donchian cooldown
    double sl_atr               = 1.0;       // SL distance = sl_atr * ATR14
    double tp_r                 = 2.5;       // TP distance = tp_r * sl_atr * ATR14
    double max_spread_pt        = 1.5;

    int    direction            = 1;         // +1 long, -1 short
    std::string timeframe       = "H1";
    std::string symbol          = "XAUUSD";
    std::string cell_id         = "Donchian_H1_long";

    // ---- Rolling Donchian channel: highs/lows windows ----------------------
    std::deque<double> highs_;
    std::deque<double> lows_;

    // ---- Position state -----------------------------------------------------
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

    // ---- Diagnostics --------------------------------------------------------
    int    bar_count_           = 0;
    int    signal_cooldown_left_ = 0;        // decrements every bar regardless

    bool has_open_position() const noexcept { return pos_active_; }

    // -------------------------------------------------------------------------
    //  on_bar -- single completed parent bar.
    //  Returns 1 if a NEW position was opened this bar, 0 otherwise.
    // -------------------------------------------------------------------------
    int on_bar(const DonchianBar& b, double bid, double ask, double atr14_at_signal,
               int64_t now_ms, double size_lot, OnCloseCb on_close) noexcept {
        ++bar_count_;

        // Capture prior channel BEFORE pushing this bar (matches sig_donchian's
        // rolling().shift(1) semantics).
        const bool channel_ready = ((int)highs_.size() >= period);
        double prior_high = 0.0;
        double prior_low  = 0.0;
        if (channel_ready) {
            prior_high = *std::max_element(highs_.begin(), highs_.end());
            prior_low  = *std::min_element(lows_.begin(),  lows_.end());
        }

        // Push this bar into the rolling window.
        highs_.push_back(b.high);
        lows_ .push_back(b.low);
        const std::size_t cap = static_cast<std::size_t>(period);
        while (highs_.size() > cap) highs_.pop_front();
        while (lows_ .size() > cap) lows_ .pop_front();

        // Decrement signal cooldown on every bar (regardless of position).
        if (signal_cooldown_left_ > 0) --signal_cooldown_left_;

        // ----- Manage existing position FIRST ------------------------------
        if (pos_active_) {
            ++pos_bars_held_;

            const double max_move_this_bar = direction == 1
                ? (b.high - pos_entry_) : (pos_entry_ - b.low);
            const double min_move_this_bar = direction == 1
                ? (b.low  - pos_entry_) : (pos_entry_ - b.high);
            if (max_move_this_bar > pos_mfe_) pos_mfe_ = max_move_this_bar;
            if (min_move_this_bar < pos_mae_) pos_mae_ = min_move_this_bar;

            // Check SL first (matches sim_a's intrabar order: hit_sl then hit_tp)
            const bool sl_hit = direction == 1
                ? (b.low  <= pos_sl_) : (b.high >= pos_sl_);
            if (sl_hit) {
                _close(pos_sl_, "SL_HIT", now_ms, on_close);
                return 0;
            }
            const bool tp_hit = direction == 1
                ? (b.high >= pos_tp_) : (b.low  <= pos_tp_);
            if (tp_hit) {
                _close(pos_tp_, "TP_HIT", now_ms, on_close);
                return 0;
            }
            if (pos_bars_held_ >= max_hold_bars) {
                _close(b.close, "TIME_EXIT", now_ms, on_close);
                return 0;
            }
            return 0;
        }

        // ----- Pre-fire gates ---------------------------------------------
        if (!enabled)                                               return 0;
        if (!channel_ready)                                         return 0;
        if (signal_cooldown_left_ > 0)                              return 0;
        if (!std::isfinite(atr14_at_signal) || atr14_at_signal <= 0.0) return 0;
        const double spread_pt = ask - bid;
        if (!std::isfinite(spread_pt) || spread_pt < 0.0)           return 0;
        if (spread_pt > max_spread_pt)                              return 0;
        if (size_lot <= 0.0)                                        return 0;

        // ----- Donchian breakout signal -----------------------------------
        const int sig_dir =
            (b.close > prior_high) ? +1 :
            (b.close < prior_low ) ? -1 : 0;
        if (sig_dir == 0)                                           return 0;

        // ALWAYS arm the signal cooldown when sig_dir != 0 (mirrors Python
        // sig_donchian: cooldown applied at signal-detection level, not
        // direction-conditional).
        signal_cooldown_left_ = signal_cooldown_bars;

        if (sig_dir != direction)                                   return 0;

        // ----- Open the position -----------------------------------------
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
               " ch_hi=%.2f ch_lo=%.2f close=%.2f spread=%.2f%s\n",
               cell_id.c_str(),
               direction == 1 ? "LONG" : "SHORT",
               entry_px, sl_px, tp_px, pos_size_, atr14_at_signal,
               prior_high, prior_low, b.close, spread_pt,
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

        // Tick-level SL fill, then TP fill.
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
            tr.regime        = "DONCHIAN";
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
//  DonchianPortfolio -- 7 cells: H2 long, H4 long+short, H6 long+short,
//  D1 long+short. (H1 long is NOT here -- it's the retuned C1Retuned cell.)
// =============================================================================
struct DonchianPortfolio {
    using OnCloseCb = std::function<void(const TradeRecord&)>;

    bool   enabled           = true;
    bool   shadow_mode       = true;
    int    max_concurrent    = 7;
    double risk_pct          = 0.005;
    double start_equity      = 10000.0;
    double margin_call       = 1000.0;
    double max_lot_cap       = 0.05;
    bool   block_on_risk_off = true;
    std::string warmup_csv_path = "";

    DonchianCell h2_long_;
    DonchianCell h4_long_;
    DonchianCell h4_short_;
    DonchianCell h6_long_;
    DonchianCell h6_short_;
    DonchianCell d1_long_;
    DonchianCell d1_short_;

    DonchianBarSynth synth_h2_;     // stride 2
    DonchianBarSynth synth_h4_;     // stride 4
    DonchianBarSynth synth_h6_;     // stride 6
    DonchianBarSynth synth_d1_;     // stride 24

    DonchianATR14 atr_h2_;
    DonchianATR14 atr_h4_;
    DonchianATR14 atr_h6_;
    DonchianATR14 atr_d1_;

    double equity_                 = 10000.0;
    double peak_equity_            = 10000.0;
    double max_dd_pct_             = 0.0;
    int    open_count_             = 0;
    int    blocked_max_concurrent_ = 0;
    int    blocked_risk_off_       = 0;

    std::string macro_regime_      = "NEUTRAL";
    void set_macro_regime(const std::string& r) noexcept { macro_regime_ = r; }

    void init() noexcept {
        // Configure cells. H2 long uses sl_atr=1.0,tp_r=2.5 per Python catalog.
        // H4/H6/D1 long+short all use the same default (sl_atr=1.0, tp_r=2.5).
        auto stamp = [](DonchianCell& c, const char* tf, int dir,
                        const char* id) {
            c.symbol     = "XAUUSD";
            c.cell_id    = id;
            c.timeframe  = tf;
            c.direction  = dir;
            c.period     = 20;
            c.sl_atr     = 1.0;
            c.tp_r       = 2.5;
            c.max_hold_bars = 30;
            c.signal_cooldown_bars = 5;
        };
        stamp(h2_long_,  "H2", +1, "Donchian_H2_long");
        stamp(h4_long_,  "H4", +1, "Donchian_H4_long");
        stamp(h4_short_, "H4", -1, "Donchian_H4_short");
        stamp(h6_long_,  "H6", +1, "Donchian_H6_long");
        stamp(h6_short_, "H6", -1, "Donchian_H6_short");
        stamp(d1_long_,  "D1", +1, "Donchian_D1_long");
        stamp(d1_short_, "D1", -1, "Donchian_D1_short");

        DonchianCell* cells[] = { &h2_long_, &h4_long_, &h4_short_, &h6_long_,
                                  &h6_short_, &d1_long_, &d1_short_ };
        for (DonchianCell* c : cells) c->shadow_mode = shadow_mode;

        synth_h2_.stride =  2;
        synth_h4_.stride =  4;
        synth_h6_.stride =  6;
        synth_d1_.stride = 24;

        equity_      = start_equity;
        peak_equity_ = start_equity;

        printf("[DONCHIAN] DonchianPortfolio ARMED (shadow_mode=%s, enabled=%s) "
               "cells=H2_long, H4_long+short, H6_long+short, D1_long+short "
               "period=20 sl_atr=1.0 tp_r=2.5 max_hold=30 cooldown=5 "
               "risk_pct=%.4f max_lot_cap=%.3f max_concurrent=%d "
               "block_on_risk_off=%s equity=$%.2f\n",
               shadow_mode ? "true" : "false",
               enabled     ? "true" : "false",
               risk_pct, max_lot_cap, max_concurrent,
               block_on_risk_off ? "true" : "false",
               equity_);
        for (DonchianCell* c : cells) {
            printf("[%s] ARMED (shadow_mode=%s, period=%d, sl=%.1f*atr, tp=%.2f*atr, max_hold=%d)\n",
                   c->cell_id.c_str(),
                   c->shadow_mode ? "true" : "false",
                   c->period, c->sl_atr, c->sl_atr * c->tp_r,
                   c->max_hold_bars);
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

            printf("[DONCHIAN-CLOSE] cell=%s pnl=$%.2f equity=$%.2f peak=$%.2f"
                   " dd=%.2f%% open=%d\n",
                   tr.engine.c_str(), pnl_usd, equity_, peak_equity_,
                   max_dd_pct_ * 100.0, open_count_);
            fflush(stdout);

            if (runtime_cb) runtime_cb(tr);
        };
    }

    void _drive_cell(DonchianCell& cell, const DonchianBar& b,
                     double bid, double ask, double atr14, int cell_idx,
                     int64_t now_ms, OnCloseCb runtime_cb) noexcept {
        if (cell.has_open_position()) {
            (void)cell.on_bar(b, bid, ask, atr14, now_ms, 0.0,
                              wrap(runtime_cb, cell_idx));
            return;
        }
        if (!can_open()) {
            if (block_on_risk_off && macro_regime_ == "RISK_OFF")
                ++blocked_risk_off_;
            else
                ++blocked_max_concurrent_;
            (void)cell.on_bar(b, bid, ask, atr14, now_ms, 0.0,
                              wrap(runtime_cb, cell_idx));
            return;
        }
        const double sl_pts = atr14 * cell.sl_atr;
        const double lot    = size_for(sl_pts);
        const int    opened = cell.on_bar(b, bid, ask, atr14, now_ms, lot,
                                          wrap(runtime_cb, cell_idx));
        if (opened) ++open_count_;
    }

    // -------------------------------------------------------------------------
    //  on_h1_bar -- single integration point. Drives H2/H4/H6/D1 cells from
    //  the H1 stream via internal synthesis. (No H1 cell here -- the retuned
    //  H1 long is in C1RetunedPortfolio.)
    // -------------------------------------------------------------------------
    void on_h1_bar(const DonchianBar& h1, double bid, double ask,
                   double /*h1_atr14_unused*/,
                   int64_t now_ms, OnCloseCb runtime_cb) noexcept {
        if (!enabled) return;

        // ---- H2 cell ----
        synth_h2_.on_h1_bar(h1, [&](const DonchianBar& b) {
            atr_h2_.on_bar(b);
            if (atr_h2_.ready()) _drive_cell(h2_long_,  b, bid, ask, atr_h2_.value(), 0, now_ms, runtime_cb);
            else                 (void)h2_long_ .on_bar(b, bid, ask, 0.0, now_ms, 0.0, wrap(runtime_cb, 0));
        });

        // ---- H4 cells (long + short share the same synth/ATR) ----
        synth_h4_.on_h1_bar(h1, [&](const DonchianBar& b) {
            atr_h4_.on_bar(b);
            const double a = atr_h4_.value();
            const bool   r = atr_h4_.ready();
            if (r) {
                _drive_cell(h4_long_,  b, bid, ask, a, 1, now_ms, runtime_cb);
                _drive_cell(h4_short_, b, bid, ask, a, 2, now_ms, runtime_cb);
            } else {
                (void)h4_long_ .on_bar(b, bid, ask, 0.0, now_ms, 0.0, wrap(runtime_cb, 1));
                (void)h4_short_.on_bar(b, bid, ask, 0.0, now_ms, 0.0, wrap(runtime_cb, 2));
            }
        });

        // ---- H6 cells ----
        synth_h6_.on_h1_bar(h1, [&](const DonchianBar& b) {
            atr_h6_.on_bar(b);
            const double a = atr_h6_.value();
            const bool   r = atr_h6_.ready();
            if (r) {
                _drive_cell(h6_long_,  b, bid, ask, a, 3, now_ms, runtime_cb);
                _drive_cell(h6_short_, b, bid, ask, a, 4, now_ms, runtime_cb);
            } else {
                (void)h6_long_ .on_bar(b, bid, ask, 0.0, now_ms, 0.0, wrap(runtime_cb, 3));
                (void)h6_short_.on_bar(b, bid, ask, 0.0, now_ms, 0.0, wrap(runtime_cb, 4));
            }
        });

        // ---- D1 cells ----
        synth_d1_.on_h1_bar(h1, [&](const DonchianBar& b) {
            atr_d1_.on_bar(b);
            const double a = atr_d1_.value();
            const bool   r = atr_d1_.ready();
            if (r) {
                _drive_cell(d1_long_,  b, bid, ask, a, 5, now_ms, runtime_cb);
                _drive_cell(d1_short_, b, bid, ask, a, 6, now_ms, runtime_cb);
            } else {
                (void)d1_long_ .on_bar(b, bid, ask, 0.0, now_ms, 0.0, wrap(runtime_cb, 5));
                (void)d1_short_.on_bar(b, bid, ask, 0.0, now_ms, 0.0, wrap(runtime_cb, 6));
            }
        });
    }

    void on_tick(double bid, double ask, int64_t now_ms,
                 OnCloseCb runtime_cb) noexcept {
        if (!enabled) return;
        h2_long_ .on_tick(bid, ask, now_ms, wrap(runtime_cb, 0));
        h4_long_ .on_tick(bid, ask, now_ms, wrap(runtime_cb, 1));
        h4_short_.on_tick(bid, ask, now_ms, wrap(runtime_cb, 2));
        h6_long_ .on_tick(bid, ask, now_ms, wrap(runtime_cb, 3));
        h6_short_.on_tick(bid, ask, now_ms, wrap(runtime_cb, 4));
        d1_long_ .on_tick(bid, ask, now_ms, wrap(runtime_cb, 5));
        d1_short_.on_tick(bid, ask, now_ms, wrap(runtime_cb, 6));
    }

    void force_close_all(double bid, double ask, int64_t now_ms,
                         OnCloseCb runtime_cb) noexcept {
        h2_long_ .force_close(bid, ask, now_ms, wrap(runtime_cb, 0));
        h4_long_ .force_close(bid, ask, now_ms, wrap(runtime_cb, 1));
        h4_short_.force_close(bid, ask, now_ms, wrap(runtime_cb, 2));
        h6_long_ .force_close(bid, ask, now_ms, wrap(runtime_cb, 3));
        h6_short_.force_close(bid, ask, now_ms, wrap(runtime_cb, 4));
        d1_long_ .force_close(bid, ask, now_ms, wrap(runtime_cb, 5));
        d1_short_.force_close(bid, ask, now_ms, wrap(runtime_cb, 6));
    }

    int total_open() const noexcept {
        return (h2_long_ .has_open_position() ? 1 : 0)
             + (h4_long_ .has_open_position() ? 1 : 0)
             + (h4_short_.has_open_position() ? 1 : 0)
             + (h6_long_ .has_open_position() ? 1 : 0)
             + (h6_short_.has_open_position() ? 1 : 0)
             + (d1_long_ .has_open_position() ? 1 : 0)
             + (d1_short_.has_open_position() ? 1 : 0);
    }

    // -------------------------------------------------------------------------
    //  warmup_from_csv -- mirrors TsmomPortfolio::warmup_from_csv. Same CSV
    //  format: bar_start_ms,open,high,low,close. Reuses the tsmom warmup CSV
    //  (phase1/signal_discovery/tsmom_warmup_H1.csv) -- both engines need the
    //  exact same H1 stream input.
    // -------------------------------------------------------------------------
    int warmup_from_csv(const std::string& path) noexcept {
        if (!enabled) {
            printf("[DONCHIAN-WARMUP] skipped -- portfolio disabled\n");
            fflush(stdout); return 0;
        }
        if (path.empty()) {
            printf("[DONCHIAN-WARMUP] skipped -- warmup_csv_path empty (cold start)\n");
            fflush(stdout); return 0;
        }
        std::ifstream f(path);
        if (!f.is_open()) {
            printf("[DONCHIAN-WARMUP] FAIL -- cannot open '%s' (cold start)\n",
                   path.c_str());
            fflush(stdout); return 0;
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

            DonchianBar b;
            b.bar_start_ms = static_cast<int64_t>(ms_ll);
            b.open = o; b.high = h; b.low = l; b.close = c;
            if (fed == 0) first_ms = b.bar_start_ms;
            last_ms = b.bar_start_ms;
            _feed_warmup_h1_bar(b);
            ++fed;
        }

        printf("[DONCHIAN-WARMUP] fed=%d rejected=%d first_ms=%lld last_ms=%lld path='%s'\n",
               fed, rejected,
               static_cast<long long>(first_ms),
               static_cast<long long>(last_ms),
               path.c_str());
        printf("[DONCHIAN-WARMUP] cell readiness: "
               "H2 hi/lo=%d/%d  H4 hi/lo=%d/%d atr=%s  "
               "H6 hi/lo=%d/%d atr=%s  D1 hi/lo=%d/%d atr=%s\n",
               static_cast<int>(h2_long_.highs_.size()), h2_long_.period,
               static_cast<int>(h4_long_.highs_.size()), h4_long_.period,
               atr_h4_.ready() ? "ready" : "cold",
               static_cast<int>(h6_long_.highs_.size()), h6_long_.period,
               atr_h6_.ready() ? "ready" : "cold",
               static_cast<int>(d1_long_.highs_.size()), d1_long_.period,
               atr_d1_.ready() ? "ready" : "cold");
        fflush(stdout);
        return fed;
    }

private:
    void _feed_warmup_h1_bar(const DonchianBar& h1) noexcept {
        auto noop_cb = OnCloseCb{};
        const double bid    = h1.close;
        const double ask    = h1.close;
        const int64_t now_ms = h1.bar_start_ms;

        synth_h2_.on_h1_bar(h1, [&](const DonchianBar& b) {
            atr_h2_.on_bar(b);
            (void)h2_long_ .on_bar(b, bid, ask, atr_h2_.value(), now_ms, 0.0, noop_cb);
        });
        synth_h4_.on_h1_bar(h1, [&](const DonchianBar& b) {
            atr_h4_.on_bar(b);
            const double a = atr_h4_.value();
            (void)h4_long_ .on_bar(b, bid, ask, a, now_ms, 0.0, noop_cb);
            (void)h4_short_.on_bar(b, bid, ask, a, now_ms, 0.0, noop_cb);
        });
        synth_h6_.on_h1_bar(h1, [&](const DonchianBar& b) {
            atr_h6_.on_bar(b);
            const double a = atr_h6_.value();
            (void)h6_long_ .on_bar(b, bid, ask, a, now_ms, 0.0, noop_cb);
            (void)h6_short_.on_bar(b, bid, ask, a, now_ms, 0.0, noop_cb);
        });
        synth_d1_.on_h1_bar(h1, [&](const DonchianBar& b) {
            atr_d1_.on_bar(b);
            const double a = atr_d1_.value();
            (void)d1_long_ .on_bar(b, bid, ask, a, now_ms, 0.0, noop_cb);
            (void)d1_short_.on_bar(b, bid, ask, a, now_ms, 0.0, noop_cb);
        });
    }
};

} // namespace omega
