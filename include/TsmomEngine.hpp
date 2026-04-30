// =============================================================================
//  TsmomEngine.hpp -- Tier-1 ship: 5 long tsmom cells (H1 / H2 / H4 / H6 / D1)
//  for live shadow paper-trading on XAUUSD.
//
//  Created 2026-04-30. Updated 2026-04-30 PM with MULTI-POSITION support.
//  Verdict source:
//      phase1/signal_discovery/POST_CUT_FULL_REPORT.md
//      phase1/signal_discovery/post_cut_revalidate_all.py (sig_tsmom + sim_c)
//
//  POST-CUT BACKTEST (post-2025-04 corpus, 1 unit, 365 days, sim_c, costs=0.65pt
//  spread + 0.05pt commission):
//      tsmom H1 long: 3,484 trades, 53.2% WR, +$17,482, $5.02/trade,  pf 1.39
//      tsmom H2 long: 1,826 trades, 55.1% WR, +$12,952, $7.09/trade,  pf 1.35
//      tsmom H4 long:   933 trades, 61.4% WR, +$15,885, $17.03/trade, pf 1.66
//      tsmom H6 long:   661 trades, 57.8% WR, +$13,380, $20.24/trade, pf 1.65
//      tsmom D1 long:   216 trades, 56.5% WR,  +$9,109, $42.17/trade, pf 1.65
//  These 5 cells = 82% of the simulated portfolio edge across the 27-cell
//  master_summary survivor set.
//
//  WHY MULTI-POSITION (audit-fixes-25 change, 2026-04-30 PM):
//  -----------------------------------------------------------
//  The Python sim_c opens a NEW position for every non-zero signal (positions
//  overlap freely). The original single-position TsmomCell retained ~10% of
//  the report's trade count and ~8% of its PnL because most signals fired
//  while a previous position was still open and got dropped by the
//  single-position constraint. Per-trade edge (WR / PF / avg) was preserved
//  but total edge was massively under-captured.
//
//  Retest data (post_cut_revalidate vs single-pos engine, 1yr H1):
//      Python sim_c (multi-position):  3,484 trades, +$17,482
//      Engine (single-pos, cd=0):        367 trades, +$ 1,744 (10.0%)
//      Engine (single-pos, cd=12):       205 trades, +$ 1,046 ( 6.0%)
//
//  Multi-position recovers the dropped fires up to a configurable cap
//  (max_positions_per_cell, default 10). With 10 concurrent positions per
//  cell the engine matches sim_c semantics. Each position is independently
//  sized via the same risk-budget formula -- so portfolio risk grows
//  linearly in the number of concurrent positions, but each position is
//  always capped at max_lot_cap.
//
//  RISK ENVELOPE WITH MULTI-POSITION:
//      worst-case per-cell drawdown if all positions hit SL simultaneously:
//          max_positions_per_cell * max_lot_cap * sl_pts * USD_per_pt_per_lot
//      For default 10 * 0.05 * 6 * $100 = $300 per cell = 3% of $10K equity
//      Across all 5 cells, worst case = 15% drawdown
//      Realistic case (uncorrelated SL hits): much lower
//
//      To tighten: lower max_positions_per_cell (e.g. 5), or lower
//      max_lot_cap, or lower risk_pct. All three options are config-tunable.
//
//  SIGNAL (mirrors phase1/signal_discovery/post_cut_revalidate_all.py::sig_tsmom):
//      ret_n = close[t] - close[t - lookback]    # lookback = 20
//      side  = +1 if ret_n > 0    (long fire)
//              -1 if ret_n < 0    (short fire -- not used in Tier-1)
//
//  EXIT (mirrors sim_c, applied INDEPENDENTLY to each open position):
//      Entry at NEXT bar open after signal close.
//      Hard SL: entry +/- 3.0 * ATR14_at_signal_for_THIS_position
//      Time exit: hold_bars=12 bars after THIS position's entry.
//      No TP.
//
//  COOLDOWN: with multi-position semantics, the legacy cooldown_bars field
//  is now defaulted to 0 (no post-trade cooldown). Multi-position is the
//  intended throttle. Keep cooldown_bars > 0 if you specifically want to
//  rate-limit the entry path (1 fire per N bars regardless of how many
//  positions are open or have closed).
//
//  HOW THIS INTEGRATES WITH THE EXISTING RUNTIME (unchanged from prior version):
//  ------------------------------------------------------------------------------
//      1. globals.hpp        : declare static omega::TsmomPortfolio g_tsmom;
//      2. engine_init.hpp    : configure g_tsmom and call g_tsmom.init() inside
//                              init_engines(), then warmup_from_csv().
//      3. tick_gold.hpp      : call g_tsmom.on_h1_bar(...) on H1 close,
//                              g_tsmom.on_tick(...) on every XAUUSD tick.
//      4. omega_config.ini   : [tsmom] section + 5 [tsmom_<TF>_long] sub-sections.
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
#include <vector>
#include "OmegaTradeLedger.hpp"
#include "CellPrimitives.hpp"   // Phase 1 of CellEngine refactor; layout sanity asserts below

namespace omega {

// =============================================================================
//  TsmomBar / TsmomBarSynth / TsmomATR14 -- shared helpers (unchanged)
// =============================================================================
struct TsmomBar {
    int64_t bar_start_ms = 0;
    double  open  = 0.0;
    double  high  = 0.0;
    double  low   = 0.0;
    double  close = 0.0;
};

struct TsmomBarSynth {
    int      stride       = 1;
    int      accum_count_ = 0;
    TsmomBar cur_{};
    using EmitCallback = std::function<void(const TsmomBar&)>;

    void on_h1_bar(const TsmomBar& h1, EmitCallback emit) noexcept {
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
            cur_         = TsmomBar{};
        }
    }
};

struct TsmomATR14 {
    static constexpr int ATR_P = 14;
    bool   has_prev_close_ = false;
    double prev_close_     = 0.0;
    double atr_            = 0.0;
    int    n_              = 0;

    bool ready() const noexcept { return n_ >= ATR_P; }

    void on_bar(const TsmomBar& b) noexcept {
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

// -----------------------------------------------------------------------------
//  Phase 1 structural-sanity gate (refactor plan §4 Phase 1).
//  These asserts confirm TsmomBar/TsmomBarSynth/TsmomATR14 are layout-
//  compatible with the canonical omega::cell types declared in
//  CellPrimitives.hpp. Any drift breaks the V1/V2 shadow comparison that
//  Phase 2 depends on, so we want a hard compile-time stop here -- not a
//  runtime divergence later. Pure additive: no behaviour change.
// -----------------------------------------------------------------------------
static_assert(sizeof(TsmomBar)      == sizeof(::omega::cell::Bar),
              "TsmomBar size drift vs omega::cell::Bar");
static_assert(sizeof(TsmomBarSynth) == sizeof(::omega::cell::BarSynth),
              "TsmomBarSynth size drift vs omega::cell::BarSynth");
static_assert(sizeof(TsmomATR14)    == sizeof(::omega::cell::ATR14),
              "TsmomATR14 size drift vs omega::cell::ATR14");

// =============================================================================
//  TsmomCell -- multi-position per cell. positions_ is a vector of open
//  positions; each evolves independently via SL or TIME_EXIT. Up to
//  max_positions_per_cell concurrent positions per cell.
// =============================================================================
struct TsmomCell {
    using OnCloseCb = std::function<void(const TradeRecord&)>;

    // ---- Per-position state -------------------------------------------------
    struct Position {
        double  entry         = 0.0;
        double  sl            = 0.0;
        double  size          = 0.0;
        double  atr           = 0.0;
        int64_t entry_ms      = 0;
        int     bars_held     = 0;
        double  mfe           = 0.0;     // signed best move (>=0 typical)
        double  mae           = 0.0;     // signed worst move (<=0 typical)
        double  spread_at     = 0.0;
        int     id            = 0;
    };

    // ---- Configuration (set at init; immutable during run) ------------------
    bool   shadow_mode             = true;
    bool   enabled                 = true;
    int    lookback                = 20;
    int    hold_bars               = 12;
    int    max_positions_per_cell  = 10;     // matches sim_c overlap
    int    cooldown_bars           = 0;      // legacy; default 0 with multi-pos
    double hard_sl_atr             = 3.0;
    double max_spread_pt           = 1.5;

    int    direction       = 1;
    std::string timeframe  = "H1";
    std::string symbol     = "XAUUSD";
    std::string cell_id    = "Tsmom_H1_long";

    // ---- Rolling closes window ---------------------------------------------
    std::deque<double> closes_;

    // ---- Open positions (multi) --------------------------------------------
    std::vector<Position> positions_;
    int trade_id_       = 0;
    int bar_count_      = 0;
    int cooldown_left_  = 0;     // decremented each bar; gates new entries

    bool has_open_position() const noexcept { return !positions_.empty(); }
    int  n_open()             const noexcept { return (int)positions_.size(); }

    // -------------------------------------------------------------------------
    //  on_bar -- called once per completed parent bar. Manages all open
    //  positions, then evaluates signal and (if room) opens a new one.
    //
    //  Returns 1 if a NEW position was opened this bar, 0 otherwise.
    // -------------------------------------------------------------------------
    int on_bar(const TsmomBar& b, double bid, double ask, double atr14_at_signal,
               int64_t now_ms, double size_lot, OnCloseCb on_close) noexcept {
        ++bar_count_;

        // ----- 1. Manage all open positions (in-place, removing closed) -----
        for (auto it = positions_.begin(); it != positions_.end(); ) {
            Position& p = *it;
            ++p.bars_held;

            const double max_move_this_bar = direction == 1
                ? (b.high - p.entry) : (p.entry - b.low);
            const double min_move_this_bar = direction == 1
                ? (b.low  - p.entry) : (p.entry - b.high);
            if (max_move_this_bar > p.mfe) p.mfe = max_move_this_bar;
            if (min_move_this_bar < p.mae) p.mae = min_move_this_bar;

            const bool sl_hit = direction == 1
                ? (b.low  <= p.sl) : (b.high >= p.sl);
            if (sl_hit) {
                _close(p, p.sl, "SL_HIT", now_ms, on_close);
                it = positions_.erase(it);
                continue;
            }
            if (p.bars_held >= hold_bars) {
                _close(p, b.close, "TIME_EXIT", now_ms, on_close);
                it = positions_.erase(it);
                continue;
            }
            ++it;
        }

        // ----- 2. Update rolling closes window ----------------------------
        closes_.push_back(b.close);
        const std::size_t cap = static_cast<std::size_t>(lookback) + 1;
        while (closes_.size() > cap) closes_.pop_front();

        // ----- 3. Decrement cooldown (legacy throttle; default 0) ---------
        if (cooldown_left_ > 0) --cooldown_left_;

        // ----- 4. Pre-fire gates -----------------------------------------
        if (!enabled)                                                  return 0;
        if (n_open() >= max_positions_per_cell)                        return 0;
        if (cooldown_left_ > 0)                                        return 0;
        if ((int)closes_.size() < lookback + 1)                        return 0;
        if (!std::isfinite(atr14_at_signal) || atr14_at_signal <= 0.0) return 0;
        const double spread_pt = ask - bid;
        if (!std::isfinite(spread_pt) || spread_pt < 0.0)              return 0;
        if (spread_pt > max_spread_pt)                                 return 0;
        if (size_lot <= 0.0)                                           return 0;

        // ----- 5. tsmom signal -------------------------------------------
        const double cur     = closes_.back();
        const double earlier = closes_[closes_.size() - 1 - lookback];
        const double ret_n   = cur - earlier;

        const int sig_dir =
            (ret_n > 0.0) ? +1 :
            (ret_n < 0.0) ? -1 : 0;
        if (sig_dir == 0)             return 0;
        if (sig_dir != direction)     return 0;   // Tier-1: long-only

        // ----- 6. Open the position --------------------------------------
        const double entry_px = direction == 1 ? ask : bid;
        const double sl_pts   = atr14_at_signal * hard_sl_atr;
        const double sl_px    = entry_px - direction * sl_pts;

        Position p;
        p.entry      = entry_px;
        p.sl         = sl_px;
        p.size       = size_lot;
        p.atr        = atr14_at_signal;
        p.entry_ms   = now_ms;
        p.bars_held  = 0;
        p.mfe        = 0.0;
        p.mae        = 0.0;
        p.spread_at  = spread_pt;
        p.id         = ++trade_id_;
        positions_.push_back(p);

        if (cooldown_bars > 0) cooldown_left_ = cooldown_bars;

        printf("[%s] ENTRY %s @ %.2f sl=%.2f size=%.4f atr=%.3f"
               " ret_n=%.3f spread=%.2f n_open=%d/%d%s\n",
               cell_id.c_str(),
               direction == 1 ? "LONG" : "SHORT",
               entry_px, sl_px, p.size, atr14_at_signal,
               ret_n, spread_pt,
               n_open(), max_positions_per_cell,
               shadow_mode ? " [SHADOW]" : "");
        fflush(stdout);
        return 1;
    }

    // -------------------------------------------------------------------------
    //  on_tick -- intrabar SL fills for ALL open positions. Iterates the
    //  vector and erases any position whose tick-level SL was hit.
    // -------------------------------------------------------------------------
    void on_tick(double bid, double ask, int64_t now_ms,
                 OnCloseCb on_close) noexcept {
        for (auto it = positions_.begin(); it != positions_.end(); ) {
            Position& p = *it;
            const double signed_move = direction == 1
                ? (bid - p.entry) : (p.entry - ask);
            if (signed_move > p.mfe) p.mfe = signed_move;
            if (signed_move < p.mae) p.mae = signed_move;

            bool hit = false;
            if (direction == 1) {
                if (bid <= p.sl) {
                    _close(p, bid, "SL_HIT", now_ms, on_close);
                    hit = true;
                }
            } else {
                if (ask >= p.sl) {
                    _close(p, ask, "SL_HIT", now_ms, on_close);
                    hit = true;
                }
            }

            if (hit) it = positions_.erase(it);
            else     ++it;
        }
    }

    // -------------------------------------------------------------------------
    //  force_close -- shutdown / weekend-gap exit for ALL open positions.
    // -------------------------------------------------------------------------
    void force_close(double bid, double ask, int64_t now_ms,
                     OnCloseCb on_close) noexcept {
        const double exit_px = direction == 1 ? bid : ask;
        for (Position& p : positions_) {
            _close(p, exit_px, "FORCE_CLOSE", now_ms, on_close);
        }
        positions_.clear();
    }

private:
    void _close(Position& p, double exit_px, const char* reason,
                int64_t now_ms, OnCloseCb on_close) noexcept {
        const double pnl_pts = (exit_px - p.entry) * direction * p.size;

        printf("[%s] EXIT %s @ %.2f %s pnl=$%.2f bars=%d mfe=%.1fpt mae=%.1fpt id=%d%s\n",
               cell_id.c_str(),
               direction == 1 ? "LONG" : "SHORT",
               exit_px, reason, pnl_pts * 100.0, p.bars_held,
               p.mfe, p.mae, p.id,
               shadow_mode ? " [SHADOW]" : "");
        fflush(stdout);

        if (on_close) {
            TradeRecord tr;
            tr.id            = p.id;
            tr.symbol        = symbol;
            tr.side          = direction == 1 ? "LONG" : "SHORT";
            tr.engine        = cell_id;
            tr.entryPrice    = p.entry;
            tr.exitPrice     = exit_px;
            tr.sl            = p.sl;
            tr.tp            = 0.0;
            tr.size          = p.size;
            tr.pnl           = pnl_pts;
            tr.mfe           = p.mfe * p.size;
            tr.mae           = p.mae * p.size;
            tr.entryTs       = p.entry_ms / 1000;
            tr.exitTs        = now_ms / 1000;
            tr.exitReason    = reason;
            tr.regime        = "TSMOM";
            tr.atr_at_entry  = p.atr;
            tr.spreadAtEntry = p.spread_at;
            tr.shadow        = shadow_mode;
            on_close(tr);
        }
    }
};

// =============================================================================
//  TsmomPortfolio -- orchestrator. Owns 5 long cells with multi-position
//  support. Sums total open positions across cells for portfolio-level
//  concurrency cap.
// =============================================================================
struct TsmomPortfolio {
    using OnCloseCb = std::function<void(const TradeRecord&)>;

    // ---- Configuration ------------------------------------------------------
    bool   enabled                = true;
    bool   shadow_mode            = true;
    int    max_concurrent         = 50;     // total across all cells (was 5; raised
                                            // for multi-pos default 10/cell * 5 cells)
    int    max_positions_per_cell = 10;     // per-cell cap (matches sim_c overlap)
    double risk_pct               = 0.005;
    double start_equity           = 10000.0;
    double margin_call            = 1000.0;
    double max_lot_cap            = 0.05;
    bool   block_on_risk_off      = true;

    std::string warmup_csv_path = "";

    TsmomCell h1_long_;
    TsmomCell h2_long_;
    TsmomCell h4_long_;
    TsmomCell h6_long_;
    TsmomCell d1_long_;

    TsmomBarSynth synth_h2_;
    TsmomBarSynth synth_h4_;
    TsmomBarSynth synth_h6_;
    TsmomBarSynth synth_d1_;

    TsmomATR14 atr_h1_;
    TsmomATR14 atr_h2_;
    TsmomATR14 atr_h4_;
    TsmomATR14 atr_h6_;
    TsmomATR14 atr_d1_;

    double equity_                 = 10000.0;
    double peak_equity_            = 10000.0;
    double max_dd_pct_             = 0.0;
    int    open_count_             = 0;     // recomputed from cells each on_h1
    int    blocked_max_concurrent_ = 0;
    int    blocked_risk_off_       = 0;

    std::string macro_regime_      = "NEUTRAL";
    void set_macro_regime(const std::string& r) noexcept { macro_regime_ = r; }

    int total_open() const noexcept {
        return h1_long_.n_open() + h2_long_.n_open() + h4_long_.n_open()
             + h6_long_.n_open() + d1_long_.n_open();
    }

    void init() noexcept {
        h1_long_.symbol = "XAUUSD"; h1_long_.cell_id = "Tsmom_H1_long"; h1_long_.timeframe = "H1";
        h2_long_.symbol = "XAUUSD"; h2_long_.cell_id = "Tsmom_H2_long"; h2_long_.timeframe = "H2";
        h4_long_.symbol = "XAUUSD"; h4_long_.cell_id = "Tsmom_H4_long"; h4_long_.timeframe = "H4";
        h6_long_.symbol = "XAUUSD"; h6_long_.cell_id = "Tsmom_H6_long"; h6_long_.timeframe = "H6";
        d1_long_.symbol = "XAUUSD"; d1_long_.cell_id = "Tsmom_D1_long"; d1_long_.timeframe = "D1";

        TsmomCell* cells[] = { &h1_long_, &h2_long_, &h4_long_, &h6_long_, &d1_long_ };
        for (TsmomCell* c : cells) {
            c->direction              = 1;
            c->shadow_mode            = shadow_mode;
            c->lookback               = 20;
            c->hold_bars              = 12;
            c->hard_sl_atr            = 3.0;
            c->max_positions_per_cell = max_positions_per_cell;
            c->cooldown_bars          = 0;
        }

        synth_h2_.stride =  2;
        synth_h4_.stride =  4;
        synth_h6_.stride =  6;
        synth_d1_.stride = 24;

        equity_      = start_equity;
        peak_equity_ = start_equity;

        printf("[TSMOM] TsmomPortfolio ARMED (shadow_mode=%s, enabled=%s) "
               "cells=H1,H2,H4,H6,D1 long lookback=%d hold_bars=%d "
               "sl=%.1f*ATR max_pos_per_cell=%d "
               "risk_pct=%.4f max_lot_cap=%.3f max_concurrent=%d "
               "block_on_risk_off=%s equity=$%.2f\n",
               shadow_mode ? "true" : "false",
               enabled     ? "true" : "false",
               h1_long_.lookback, h1_long_.hold_bars, h1_long_.hard_sl_atr,
               max_positions_per_cell,
               risk_pct, max_lot_cap, max_concurrent,
               block_on_risk_off ? "true" : "false",
               equity_);
        for (TsmomCell* c : cells) {
            printf("[%s] ARMED (shadow_mode=%s, lookback=%d, hold=%d, sl=%.1f*atr, max_pos=%d)\n",
                   c->cell_id.c_str(),
                   c->shadow_mode ? "true" : "false",
                   c->lookback, c->hold_bars, c->hard_sl_atr,
                   c->max_positions_per_cell);
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
        return total_open() < max_concurrent;
    }

    OnCloseCb wrap(OnCloseCb runtime_cb, int /*cell_idx*/) {
        return [this, runtime_cb](const TradeRecord& tr) {
            const double pnl_usd = tr.pnl * 100.0;
            equity_ += pnl_usd;
            if (equity_ > peak_equity_) peak_equity_ = equity_;
            const double dd = peak_equity_ > 0.0
                ? (equity_ - peak_equity_) / peak_equity_ : 0.0;
            if (dd < max_dd_pct_) max_dd_pct_ = dd;

            printf("[TSMOM-CLOSE] cell=%s pnl=$%.2f equity=$%.2f peak=$%.2f"
                   " dd=%.2f%% open=%d\n",
                   tr.engine.c_str(), pnl_usd, equity_, peak_equity_,
                   max_dd_pct_ * 100.0, total_open());
            fflush(stdout);

            if (runtime_cb) runtime_cb(tr);
        };
    }

    void _drive_cell(TsmomCell& cell, const TsmomBar& b, double bid, double ask,
                     double atr14, int cell_idx, int64_t now_ms,
                     OnCloseCb runtime_cb) noexcept {
        // Always drive on_bar -- it manages existing positions AND evaluates
        // new signals. The decision to OPEN a new position is gated inside
        // on_bar by max_positions_per_cell + size_lot value.
        const bool gate_block =
            !can_open() || cell.n_open() >= cell.max_positions_per_cell;
        const double sl_pts = atr14 * cell.hard_sl_atr;
        const double lot    = gate_block ? 0.0 : size_for(sl_pts);

        if (gate_block) {
            if (block_on_risk_off && macro_regime_ == "RISK_OFF")
                ++blocked_risk_off_;
            else
                ++blocked_max_concurrent_;
        }

        (void)cell.on_bar(b, bid, ask, atr14, now_ms, lot,
                          wrap(runtime_cb, cell_idx));
    }

    // -------------------------------------------------------------------------
    //  on_h1_bar -- single integration point. (unchanged from prior version
    //  except _drive_cell uses cell-level multi-position gating)
    // -------------------------------------------------------------------------
    void on_h1_bar(const TsmomBar& h1, double bid, double ask, double h1_atr14,
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

        synth_h2_.on_h1_bar(h1, [&](const TsmomBar& b) {
            atr_h2_.on_bar(b);
            if (atr_h2_.ready()) _drive_cell(h2_long_, b, bid, ask, atr_h2_.value(), 1, now_ms, runtime_cb);
            else                 (void)h2_long_.on_bar(b, bid, ask, 0.0, now_ms, 0.0, wrap(runtime_cb, 1));
        });
        synth_h4_.on_h1_bar(h1, [&](const TsmomBar& b) {
            atr_h4_.on_bar(b);
            if (atr_h4_.ready()) _drive_cell(h4_long_, b, bid, ask, atr_h4_.value(), 2, now_ms, runtime_cb);
            else                 (void)h4_long_.on_bar(b, bid, ask, 0.0, now_ms, 0.0, wrap(runtime_cb, 2));
        });
        synth_h6_.on_h1_bar(h1, [&](const TsmomBar& b) {
            atr_h6_.on_bar(b);
            if (atr_h6_.ready()) _drive_cell(h6_long_, b, bid, ask, atr_h6_.value(), 3, now_ms, runtime_cb);
            else                 (void)h6_long_.on_bar(b, bid, ask, 0.0, now_ms, 0.0, wrap(runtime_cb, 3));
        });
        synth_d1_.on_h1_bar(h1, [&](const TsmomBar& b) {
            atr_d1_.on_bar(b);
            if (atr_d1_.ready()) _drive_cell(d1_long_, b, bid, ask, atr_d1_.value(), 4, now_ms, runtime_cb);
            else                 (void)d1_long_.on_bar(b, bid, ask, 0.0, now_ms, 0.0, wrap(runtime_cb, 4));
        });

        open_count_ = total_open();
    }

    void on_tick(double bid, double ask, int64_t now_ms,
                 OnCloseCb runtime_cb) noexcept {
        if (!enabled) return;
        h1_long_.on_tick(bid, ask, now_ms, wrap(runtime_cb, 0));
        h2_long_.on_tick(bid, ask, now_ms, wrap(runtime_cb, 1));
        h4_long_.on_tick(bid, ask, now_ms, wrap(runtime_cb, 2));
        h6_long_.on_tick(bid, ask, now_ms, wrap(runtime_cb, 3));
        d1_long_.on_tick(bid, ask, now_ms, wrap(runtime_cb, 4));
    }

    void force_close_all(double bid, double ask, int64_t now_ms,
                         OnCloseCb runtime_cb) noexcept {
        h1_long_.force_close(bid, ask, now_ms, wrap(runtime_cb, 0));
        h2_long_.force_close(bid, ask, now_ms, wrap(runtime_cb, 1));
        h4_long_.force_close(bid, ask, now_ms, wrap(runtime_cb, 2));
        h6_long_.force_close(bid, ask, now_ms, wrap(runtime_cb, 3));
        d1_long_.force_close(bid, ask, now_ms, wrap(runtime_cb, 4));
    }

    struct Status {
        bool        enabled;
        bool        shadow_mode;
        double      equity;
        double      peak;
        double      max_dd_pct;
        int         open_count;
        int         blocked_max_concurrent;
        int         blocked_risk_off;
        std::string macro_regime;
    };
    Status status() const noexcept {
        return Status{
            enabled, shadow_mode,
            equity_, peak_equity_, max_dd_pct_,
            total_open(), blocked_max_concurrent_, blocked_risk_off_,
            macro_regime_,
        };
    }

    int warmup_from_csv(const std::string& path) noexcept {
        if (!enabled) {
            printf("[TSMOM-WARMUP] skipped -- portfolio disabled\n");
            fflush(stdout); return 0;
        }
        if (path.empty()) {
            printf("[TSMOM-WARMUP] skipped -- warmup_csv_path empty (cold start)\n");
            fflush(stdout); return 0;
        }
        std::ifstream f(path);
        if (!f.is_open()) {
            printf("[TSMOM-WARMUP] FAIL -- cannot open '%s' (cold start)\n", path.c_str());
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

            TsmomBar b;
            b.bar_start_ms = static_cast<int64_t>(ms_ll);
            b.open  = o; b.high = h; b.low = l; b.close = c;
            if (fed == 0) first_ms = b.bar_start_ms;
            last_ms = b.bar_start_ms;

            _feed_warmup_h1_bar(b);
            ++fed;
        }

        printf("[TSMOM-WARMUP] fed=%d rejected=%d first_ms=%lld last_ms=%lld path='%s'\n",
               fed, rejected,
               static_cast<long long>(first_ms),
               static_cast<long long>(last_ms),
               path.c_str());
        printf("[TSMOM-WARMUP] cell readiness: "
               "H1 closes=%d/%d  H2 closes=%d/%d atr=%s  "
               "H4 closes=%d/%d atr=%s  H6 closes=%d/%d atr=%s  "
               "D1 closes=%d/%d atr=%s\n",
               (int)h1_long_.closes_.size(), h1_long_.lookback + 1,
               (int)h2_long_.closes_.size(), h2_long_.lookback + 1,
               atr_h2_.ready() ? "ready" : "cold",
               (int)h4_long_.closes_.size(), h4_long_.lookback + 1,
               atr_h4_.ready() ? "ready" : "cold",
               (int)h6_long_.closes_.size(), h6_long_.lookback + 1,
               atr_h6_.ready() ? "ready" : "cold",
               (int)d1_long_.closes_.size(), d1_long_.lookback + 1,
               atr_d1_.ready() ? "ready" : "cold");
        fflush(stdout);
        return fed;
    }

private:
    void _feed_warmup_h1_bar(const TsmomBar& h1) noexcept {
        auto noop_cb = OnCloseCb{};
        const double bid = h1.close;
        const double ask = h1.close;
        const int64_t now_ms = h1.bar_start_ms;

        atr_h1_.on_bar(h1);
        (void)h1_long_.on_bar(h1, bid, ask, atr_h1_.value(), now_ms, 0.0, noop_cb);

        synth_h2_.on_h1_bar(h1, [&](const TsmomBar& b) {
            atr_h2_.on_bar(b);
            (void)h2_long_.on_bar(b, bid, ask, atr_h2_.value(), now_ms, 0.0, noop_cb);
        });
        synth_h4_.on_h1_bar(h1, [&](const TsmomBar& b) {
            atr_h4_.on_bar(b);
            (void)h4_long_.on_bar(b, bid, ask, atr_h4_.value(), now_ms, 0.0, noop_cb);
        });
        synth_h6_.on_h1_bar(h1, [&](const TsmomBar& b) {
            atr_h6_.on_bar(b);
            (void)h6_long_.on_bar(b, bid, ask, atr_h6_.value(), now_ms, 0.0, noop_cb);
        });
        synth_d1_.on_h1_bar(h1, [&](const TsmomBar& b) {
            atr_d1_.on_bar(b);
            (void)d1_long_.on_bar(b, bid, ask, atr_d1_.value(), now_ms, 0.0, noop_cb);
        });
    }
};

} // namespace omega
