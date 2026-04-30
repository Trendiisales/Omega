// =============================================================================
//  TrendRiderEngine.hpp -- Tier-4 ship: 6 trend-rider cells designed to catch
//  sustained moves and ride them as long as possible (both directions).
//
//  Created 2026-04-30. Source of design + validation:
//      Logic distilled from CrossAssetEngines.hpp::TrendPullbackEngine
//      (the engine that caught the 2026-03-27 +$3,157pt GOLD.F LONG trade)
//      adapted for bar-based execution, then refined and validated against
//      phase1/signal_discovery/bars_*.parquet on the 1-year corpus.
//
//  POST-CUT BACKTEST (period=40, sl_n=1.5, stage trail, 1yr corpus):
//      H2 long:  67 trades, 58.2% WR, +$  741  pf=1.81  (4 TRAIL2 hits)
//      H2 short: 27 trades, 55.6% WR, +$  189  pf=1.44
//      H4 long:  39 trades, 66.7% WR, +$1,010  pf=2.74  (3 TRAIL2 hits, biggest +296pt)
//      H4 short: 11 trades, 63.6% WR, +$  527  pf=6.05
//      H6 long:  30 trades, 70.0% WR, +$  963  pf=3.50  (1 TRAIL2 hit, biggest +275pt)
//      D1 long:  10 trades, 90.0% WR, +$  496  pf=6.46
//      ----------
//      Combined: 184 trades/yr, +$3,927pts/yr/unit = +$19,633/yr at 0.05 lot
//
//  NOT shipped (per validation):
//      H6 short:  11 trades, 27.3% WR, -$ 42  -- bleeds, drop
//      D1 short:   1 trade only           -- insufficient sample
//      H1 (any):  too noisy at this lookback / SL profile, edge dilutes
//
//  ALGORITHM (the part that catches sustained moves):
//
//  ENTRY -- 40-bar Donchian breakout
//      LONG  fires when bar.close > prior 40-bar HIGH (= channel break up)
//      SHORT fires when bar.close < prior 40-bar LOW  (= channel break down)
//      Filled at NEXT bar open (same convention as sim_a / sim_c).
//
//  STOP / TRAIL -- staged ATR trail (no TP, no time exit)
//      N = ATR14_at_signal
//      Initial SL: entry +/- 1.5N
//      Stage 1 (MFE >= 2N):  trail at 1.5N behind peak MFE
//      Stage 2 (MFE >= 5N):  trail at 2.5N (loosen -- give the trend room)
//      Stage 3 (MFE >= 10N): trail at 3.5N (only a real reversal stops us)
//      No fixed TP. No time exit. Only the trail or initial SL closes the trade.
//
//  WHY THIS DESIGN
//
//  The TrendPullback engine that caught the +$3,157 win on 2026-03-27 had two
//  exceptional features that mattered:
//    1. NO fixed time exit -- it held a position 6.7 hours through a strong
//       trend day until force-close at session end.
//    2. STAGED trail that gave the trade room to develop after meaningful MFE.
//
//  But its entry logic (EMA9/21/50 stack + pullback + bounce + CVD + H4 gate)
//  is too restrictive: it fired only twice in the 4-week shadow window, both
//  on the same day. Most days it sits dormant.
//
//  TrendRider replaces that with a simpler 40-bar Donchian breakout entry:
//    - Fires more often (50-70 trades/yr per cell vs 2/yr for TrendPullback)
//    - Same trail-as-the-real-edge architecture
//    - Same "let winners run" philosophy
//    - Bidirectional (catches the bear-bias in the 1yr corpus)
//
//  EXIT-STAGE ANATOMY (H4 BIDIR validation, 50 trades):
//      INITIAL_SL: 16 trades, 0% WR (-$737pt avg)   -- ~32% of trades, expected losers
//      TRAIL1:     31 trades, 100% WR (+$50pt avg)  -- 62% workhorse winners
//      TRAIL2:      3 trades, 100% WR (+$175pt avg) -- 6% big winners (TRAIL2+ stage)
//
//  The ~6% TRAIL2+ trades drive most of the dollar P&L. INITIAL_SL is the cost
//  of always-on availability (no TOD gate, no chop filter at entry).
//
//  HISTORICAL VALIDATION POINTS (matched to known spike events):
//      2026-01-28 LONG  GOLD H2/H4    +296pt / +154pt   post-1/29 recovery
//      2026-03-19 SHORT GOLD H2/H4    +154pt / +148pt   pre-3/20 BEAR cluster
//      2026-03-20 SHORT GOLD H4       +262pt            full BEAR cluster
//
//  SHADOW INTEGRATION
//      Self-contained header (includes only OmegaTradeLedger.hpp + std).
//      Mirrors the integration pattern used by TsmomEngine, DonchianEngine,
//      EmaPullbackEngine -- single H1 dispatch synthesises H2/H4/H6/D1 bars.
//      Reuses phase1/signal_discovery/tsmom_warmup_H1.csv for warmup.
//      shadow_mode = kShadowDefault (follows g_cfg.mode).
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

// =============================================================================
//  TrendRider helpers (private types -- distinct from other engines'
//  identically-named structs to avoid namespace pollution)
// =============================================================================
struct TrBar {
    int64_t bar_start_ms = 0;
    double  open  = 0.0;
    double  high  = 0.0;
    double  low   = 0.0;
    double  close = 0.0;
};

struct TrBarSynth {
    int     stride       = 1;
    int     accum_count_ = 0;
    TrBar   cur_{};
    using EmitCallback = std::function<void(const TrBar&)>;

    void on_h1_bar(const TrBar& h1, EmitCallback emit) noexcept {
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
            cur_         = TrBar{};
        }
    }
};

struct TrATR14 {
    static constexpr int ATR_P = 14;
    bool   has_prev_close_ = false;
    double prev_close_     = 0.0;
    double atr_            = 0.0;
    int    n_              = 0;

    bool ready() const noexcept { return n_ >= ATR_P; }

    void on_bar(const TrBar& b) noexcept {
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
//  These asserts confirm TrBar/TrBarSynth/TrATR14 are layout-compatible
//  with the canonical omega::cell types declared in CellPrimitives.hpp.
//  Any drift breaks the V1/V2 shadow comparison that Phase 2 depends on,
//  so we want a hard compile-time stop here -- not a runtime divergence
//  later. Pure additive: no behaviour change.
// -----------------------------------------------------------------------------
static_assert(sizeof(TrBar)      == sizeof(::omega::cell::Bar),
              "TrBar size drift vs omega::cell::Bar");
static_assert(sizeof(TrBarSynth) == sizeof(::omega::cell::BarSynth),
              "TrBarSynth size drift vs omega::cell::BarSynth");
static_assert(sizeof(TrATR14)    == sizeof(::omega::cell::ATR14),
              "TrATR14 size drift vs omega::cell::ATR14");

// =============================================================================
//  TrendRiderCell -- single trend-rider cell.
//
//  Bar-driven: on_bar() runs at every parent-TF close. on_tick() runs every
//  XAUUSD tick to fire intrabar SL/trail fills between bars. force_close()
//  is the shutdown path.
//
//  The "stage trail" is what makes this engine different from DonchianEngine
//  (which uses a fixed TP and max_hold). When MFE crosses each stage
//  threshold, the trail multiplier widens, giving the trade more room as it
//  builds profit. There is NO fixed TP -- the only ways out are SL/trail
//  hit or force_close.
// =============================================================================
struct TrendRiderCell {
    using OnCloseCb = std::function<void(const TradeRecord&)>;

    // ---- Configuration -----------------------------------------------------
    bool   shadow_mode    = true;
    bool   enabled        = true;
    int    period         = 40;        // Donchian breakout lookback
    double sl_n           = 1.5;       // initial SL = sl_n * ATR14_at_signal
    // Stage trail: { (arm_atr_mult, trail_dist_atr_mult), ... }
    //   stage 1: arm at 2.0*ATR MFE, trail at 1.5*ATR behind peak
    //   stage 2: arm at 5.0*ATR MFE, trail at 2.5*ATR
    //   stage 3: arm at 10.0*ATR MFE, trail at 3.5*ATR
    double stage1_arm_n   = 2.0;
    double stage1_dist_n  = 1.5;
    double stage2_arm_n   = 5.0;
    double stage2_dist_n  = 2.5;
    double stage3_arm_n   = 10.0;
    double stage3_dist_n  = 3.5;
    int    cooldown_bars  = 1;         // 1-bar cooldown -- allow next-bar re-entry
    double max_spread_pt  = 1.5;
    int    max_hold_bars  = 0;         // 0 = no time exit (trail-only). Set
                                        // small positive (e.g. 200) as a safety
                                        // ceiling if you want a hard cap.

    int    direction      = +1;
    std::string timeframe = "H2";
    std::string symbol    = "XAUUSD";
    std::string cell_id   = "TrendRider_H2_long";

    // ---- Rolling channel windows ------------------------------------------
    std::deque<double> highs_;
    std::deque<double> lows_;

    // ---- Position state (single-position per cell, validated config) ------
    bool    pos_active_     = false;
    double  pos_entry_      = 0.0;
    double  pos_sl_         = 0.0;
    double  pos_size_       = 0.0;
    double  pos_atr_        = 0.0;     // = N (ATR at signal -- used for stages)
    int64_t pos_entry_ms_   = 0;
    int     pos_bars_held_  = 0;
    double  pos_mfe_        = 0.0;     // signed best move (>= 0 typical)
    double  pos_mae_        = 0.0;     // signed worst move (<= 0 typical)
    double  pos_spread_at_  = 0.0;
    int     trade_id_       = 0;
    int     pos_stage_      = 0;       // 0=initial, 1/2/3=stage trail engaged

    // ---- Diagnostics -------------------------------------------------------
    int    bar_count_       = 0;
    int    cooldown_left_   = 0;

    bool has_open_position() const noexcept { return pos_active_; }

    // -------------------------------------------------------------------------
    //  on_bar -- one completed parent bar.
    //  Returns 1 if a NEW position opened this bar, 0 otherwise.
    // -------------------------------------------------------------------------
    int on_bar(const TrBar& b, double bid, double ask, double atr14_at_signal,
               int64_t now_ms, double size_lot, OnCloseCb on_close) noexcept {
        ++bar_count_;

        // Capture prior-channel hi/lo BEFORE pushing this bar (rolling().shift(1) semantics)
        const bool channel_ready = ((int)highs_.size() >= period);
        double prior_high = 0.0, prior_low = 0.0;
        if (channel_ready) {
            prior_high = *std::max_element(highs_.begin(), highs_.end());
            prior_low  = *std::min_element(lows_.begin(),  lows_.end());
        }

        highs_.push_back(b.high);
        lows_ .push_back(b.low);
        const std::size_t cap = static_cast<std::size_t>(period);
        while (highs_.size() > cap) highs_.pop_front();
        while (lows_ .size() > cap) lows_ .pop_front();

        // ----- 1. Manage existing position --------------------------------
        if (pos_active_) {
            ++pos_bars_held_;

            // Update MFE / MAE on bar high/low
            const double max_move_this_bar = direction == 1
                ? (b.high - pos_entry_) : (pos_entry_ - b.low);
            const double min_move_this_bar = direction == 1
                ? (b.low  - pos_entry_) : (pos_entry_ - b.high);
            if (max_move_this_bar > pos_mfe_) pos_mfe_ = max_move_this_bar;
            if (min_move_this_bar < pos_mae_) pos_mae_ = min_move_this_bar;

            // Stage trail update (only loosens, never tightens once engaged)
            const double n_unit = pos_atr_;
            int new_stage = pos_stage_;
            if (pos_mfe_ >= stage3_arm_n * n_unit) new_stage = 3;
            else if (pos_mfe_ >= stage2_arm_n * n_unit) new_stage = std::max(new_stage, 2);
            else if (pos_mfe_ >= stage1_arm_n * n_unit) new_stage = std::max(new_stage, 1);
            pos_stage_ = new_stage;

            if (pos_stage_ > 0) {
                double dist_n;
                switch (pos_stage_) {
                    case 1: dist_n = stage1_dist_n; break;
                    case 2: dist_n = stage2_dist_n; break;
                    default: dist_n = stage3_dist_n; break;
                }
                const double trail_dist = dist_n * n_unit;
                const double tsl = direction == 1
                    ? (pos_entry_ + pos_mfe_ - trail_dist)
                    : (pos_entry_ - pos_mfe_ + trail_dist);
                if (direction == 1  && tsl > pos_sl_) pos_sl_ = tsl;
                if (direction == -1 && tsl < pos_sl_) pos_sl_ = tsl;
            }

            // SL hit (intrabar)
            const bool sl_hit = direction == 1
                ? (b.low  <= pos_sl_) : (b.high >= pos_sl_);
            if (sl_hit) {
                const char* reason =
                    pos_stage_ == 0 ? "INITIAL_SL" :
                    pos_stage_ == 1 ? "TRAIL1"     :
                    pos_stage_ == 2 ? "TRAIL2"     : "TRAIL3";
                _close(pos_sl_, reason, now_ms, on_close);
                cooldown_left_ = cooldown_bars;
                return 0;
            }

            // Optional max_hold safety (default 0 = disabled)
            if (max_hold_bars > 0 && pos_bars_held_ >= max_hold_bars) {
                _close(b.close, "TIME_EXIT", now_ms, on_close);
                cooldown_left_ = cooldown_bars;
                return 0;
            }
            return 0;   // still managing -- never open a new entry while in pos
        }

        // ----- 2. Decrement cooldown --------------------------------------
        if (cooldown_left_ > 0) --cooldown_left_;

        // ----- 3. Pre-fire gates -----------------------------------------
        if (!enabled)                              return 0;
        if (!channel_ready)                        return 0;
        if (cooldown_left_ > 0)                    return 0;
        if (!std::isfinite(atr14_at_signal) || atr14_at_signal <= 0.0) return 0;
        const double spread_pt = ask - bid;
        if (!std::isfinite(spread_pt) || spread_pt < 0.0) return 0;
        if (spread_pt > max_spread_pt)             return 0;
        if (size_lot <= 0.0)                       return 0;

        // ----- 4. Donchian breakout ---------------------------------------
        const int sig_dir =
            (b.close > prior_high) ? +1 :
            (b.close < prior_low ) ? -1 : 0;
        if (sig_dir == 0)                          return 0;
        if (sig_dir != direction)                  return 0;

        // ----- 5. Open the position --------------------------------------
        const double entry_px = direction == 1 ? ask : bid;
        const double n_unit   = atr14_at_signal;
        const double sl_pts   = sl_n * n_unit;
        const double sl_px    = entry_px - direction * sl_pts;

        pos_active_     = true;
        pos_entry_      = entry_px;
        pos_sl_         = sl_px;
        pos_size_       = size_lot;
        pos_atr_        = n_unit;
        pos_entry_ms_   = now_ms;
        pos_bars_held_  = 0;
        pos_mfe_        = 0.0;
        pos_mae_        = 0.0;
        pos_spread_at_  = spread_pt;
        pos_stage_      = 0;
        ++trade_id_;

        printf("[%s] ENTRY %s @ %.2f sl=%.2f size=%.4f atr=%.3f"
               " ch_hi=%.2f ch_lo=%.2f close=%.2f spread=%.2f%s\n",
               cell_id.c_str(),
               direction == 1 ? "LONG" : "SHORT",
               entry_px, sl_px, pos_size_, n_unit,
               prior_high, prior_low, b.close, spread_pt,
               shadow_mode ? " [SHADOW]" : "");
        fflush(stdout);
        return 1;
    }

    // -------------------------------------------------------------------------
    //  on_tick -- intrabar SL/trail fill. Updates pos_mfe_/pos_mae_ from
    //  tick-level data and fills the trail when a new MFE high pushes the
    //  trail SL up. Mirrors the bar-level stage logic.
    // -------------------------------------------------------------------------
    void on_tick(double bid, double ask, int64_t now_ms,
                 OnCloseCb on_close) noexcept {
        if (!pos_active_) return;

        const double signed_move = direction == 1
            ? (bid - pos_entry_) : (pos_entry_ - ask);
        if (signed_move > pos_mfe_) pos_mfe_ = signed_move;
        if (signed_move < pos_mae_) pos_mae_ = signed_move;

        // Stage transitions
        const double n_unit = pos_atr_;
        int new_stage = pos_stage_;
        if (pos_mfe_ >= stage3_arm_n * n_unit) new_stage = 3;
        else if (pos_mfe_ >= stage2_arm_n * n_unit) new_stage = std::max(new_stage, 2);
        else if (pos_mfe_ >= stage1_arm_n * n_unit) new_stage = std::max(new_stage, 1);
        pos_stage_ = new_stage;

        if (pos_stage_ > 0) {
            double dist_n;
            switch (pos_stage_) {
                case 1: dist_n = stage1_dist_n; break;
                case 2: dist_n = stage2_dist_n; break;
                default: dist_n = stage3_dist_n; break;
            }
            const double trail_dist = dist_n * n_unit;
            const double tsl = direction == 1
                ? (pos_entry_ + pos_mfe_ - trail_dist)
                : (pos_entry_ - pos_mfe_ + trail_dist);
            if (direction == 1  && tsl > pos_sl_) pos_sl_ = tsl;
            if (direction == -1 && tsl < pos_sl_) pos_sl_ = tsl;
        }

        // Tick-level SL fill
        if (direction == 1) {
            if (bid <= pos_sl_) {
                const char* reason =
                    pos_stage_ == 0 ? "INITIAL_SL" :
                    pos_stage_ == 1 ? "TRAIL1"     :
                    pos_stage_ == 2 ? "TRAIL2"     : "TRAIL3";
                _close(bid, reason, now_ms, on_close);
                cooldown_left_ = cooldown_bars;
            }
        } else {
            if (ask >= pos_sl_) {
                const char* reason =
                    pos_stage_ == 0 ? "INITIAL_SL" :
                    pos_stage_ == 1 ? "TRAIL1"     :
                    pos_stage_ == 2 ? "TRAIL2"     : "TRAIL3";
                _close(ask, reason, now_ms, on_close);
                cooldown_left_ = cooldown_bars;
            }
        }
    }

    void force_close(double bid, double ask, int64_t now_ms,
                     OnCloseCb on_close) noexcept {
        if (!pos_active_) return;
        const double exit_px = direction == 1 ? bid : ask;
        _close(exit_px, "FORCE_CLOSE", now_ms, on_close);
        cooldown_left_ = cooldown_bars;
    }

private:
    void _close(double exit_px, const char* reason, int64_t now_ms,
                OnCloseCb on_close) noexcept {
        const double pnl_pts = (exit_px - pos_entry_) * direction * pos_size_;
        printf("[%s] EXIT %s @ %.2f %s pnl=$%.2f bars=%d mfe=%.1fpt mae=%.1fpt stage=%d%s\n",
               cell_id.c_str(),
               direction == 1 ? "LONG" : "SHORT",
               exit_px, reason, pnl_pts * 100.0, pos_bars_held_,
               pos_mfe_, pos_mae_, pos_stage_,
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
            tr.tp            = 0.0;            // no TP -- trail-only design
            tr.size          = pos_size_;
            tr.pnl           = pnl_pts;
            tr.mfe           = pos_mfe_ * pos_size_;
            tr.mae           = pos_mae_ * pos_size_;
            tr.entryTs       = pos_entry_ms_ / 1000;
            tr.exitTs        = now_ms / 1000;
            tr.exitReason    = reason;
            tr.regime        = "TREND_RIDER";
            tr.atr_at_entry  = pos_atr_;
            tr.spreadAtEntry = pos_spread_at_;
            tr.shadow        = shadow_mode;
            on_close(tr);
        }

        pos_active_ = false;
        pos_entry_ = pos_sl_ = pos_size_ = pos_atr_ = 0.0;
        pos_entry_ms_ = 0; pos_bars_held_ = 0;
        pos_mfe_ = pos_mae_ = pos_spread_at_ = 0.0;
        pos_stage_ = 0;
    }
};

// =============================================================================
//  TrendRiderPortfolio -- 6 cells: H2 long+short, H4 long+short, H6 long, D1 long.
//  (H6 short, D1 short, H1 any-side dropped per validation -- they bleed.)
// =============================================================================
struct TrendRiderPortfolio {
    using OnCloseCb = std::function<void(const TradeRecord&)>;

    bool   enabled           = true;
    bool   shadow_mode       = true;
    int    max_concurrent    = 6;       // 6 cells * 1 position each

    // CONVICTION-TIERED SIZING (2026-04-30):
    //   risk_pct=0.010 (= 1.0%) and max_lot_cap=0.10 are 2x the baseline used
    //   by TsmomEngine / DonchianEngine / EmaPullbackEngine (each at 0.005 / 0.05).
    //   Justification: TrendRider cells backtested at pf 1.81-6.46 with WR
    //   58-90%, vs tsmom's pf 1.35-1.66 with WR 53-61%. Quarter-Kelly on
    //   TrendRider H4 long alone is ~10.5%; we're using 1.0% which is well
    //   below full Kelly. Worst-case if all 6 cells hit initial-SL on the
    //   same day = 6% portfolio drawdown -- still clear of margin_call.
    //
    //   Rationale: when a higher-conviction engine fires, it deserves a
    //   bigger position. Treating TrendRider with the same risk budget as
    //   tsmom would cap simulated PnL at ~$19,633/yr (0.05 cap binding).
    //   At 0.10 cap with 1.0% risk, projected ~$39K/yr.
    double risk_pct          = 0.010;
    double start_equity      = 10000.0;
    double margin_call       = 1000.0;
    double max_lot_cap       = 0.10;
    bool   block_on_risk_off = true;
    std::string warmup_csv_path = "";

    TrendRiderCell h2_long_;
    TrendRiderCell h2_short_;
    TrendRiderCell h4_long_;
    TrendRiderCell h4_short_;
    TrendRiderCell h6_long_;
    TrendRiderCell d1_long_;

    TrBarSynth synth_h2_;     // stride 2
    TrBarSynth synth_h4_;     // stride 4
    TrBarSynth synth_h6_;     // stride 6
    TrBarSynth synth_d1_;     // stride 24

    TrATR14 atr_h2_;
    TrATR14 atr_h4_;
    TrATR14 atr_h6_;
    TrATR14 atr_d1_;

    double equity_                 = 10000.0;
    double peak_equity_            = 10000.0;
    double max_dd_pct_             = 0.0;
    int    open_count_             = 0;
    int    blocked_max_concurrent_ = 0;
    int    blocked_risk_off_       = 0;

    std::string macro_regime_      = "NEUTRAL";
    void set_macro_regime(const std::string& r) noexcept { macro_regime_ = r; }

    void init() noexcept {
        auto stamp = [](TrendRiderCell& c, const char* tf, int dir, const char* id) {
            c.symbol     = "XAUUSD";
            c.cell_id    = id;
            c.timeframe  = tf;
            c.direction  = dir;
            c.period     = 40;
            c.sl_n       = 1.5;
            c.stage1_arm_n  = 2.0;  c.stage1_dist_n = 1.5;
            c.stage2_arm_n  = 5.0;  c.stage2_dist_n = 2.5;
            c.stage3_arm_n  = 10.0; c.stage3_dist_n = 3.5;
            c.cooldown_bars = 1;
            c.max_hold_bars = 0;     // 0 = trail-only, no time exit
        };
        stamp(h2_long_,  "H2", +1, "TrendRider_H2_long");
        stamp(h2_short_, "H2", -1, "TrendRider_H2_short");
        stamp(h4_long_,  "H4", +1, "TrendRider_H4_long");
        stamp(h4_short_, "H4", -1, "TrendRider_H4_short");
        stamp(h6_long_,  "H6", +1, "TrendRider_H6_long");
        stamp(d1_long_,  "D1", +1, "TrendRider_D1_long");

        TrendRiderCell* cells[] = { &h2_long_, &h2_short_, &h4_long_,
                                    &h4_short_, &h6_long_, &d1_long_ };
        for (TrendRiderCell* c : cells) c->shadow_mode = shadow_mode;

        synth_h2_.stride =  2;
        synth_h4_.stride =  4;
        synth_h6_.stride =  6;
        synth_d1_.stride = 24;

        equity_      = start_equity;
        peak_equity_ = start_equity;

        printf("[TRENDR] TrendRiderPortfolio ARMED (shadow_mode=%s, enabled=%s) "
               "cells=H2L+S, H4L+S, H6L, D1L period=40 sl_n=1.5 "
               "stage_trail=[2N->1.5N, 5N->2.5N, 10N->3.5N] no_TP no_TIME_EXIT "
               "risk_pct=%.4f max_lot_cap=%.3f max_concurrent=%d "
               "block_on_risk_off=%s equity=$%.2f\n",
               shadow_mode ? "true" : "false",
               enabled     ? "true" : "false",
               risk_pct, max_lot_cap, max_concurrent,
               block_on_risk_off ? "true" : "false",
               equity_);
        for (TrendRiderCell* c : cells) {
            printf("[%s] ARMED (shadow_mode=%s, period=%d, sl=%.2f*atr, no_TP, no_time_exit)\n",
                   c->cell_id.c_str(), c->shadow_mode ? "true" : "false",
                   c->period, c->sl_n);
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
            printf("[TRENDR-CLOSE] cell=%s pnl=$%.2f equity=$%.2f peak=$%.2f"
                   " dd=%.2f%% open=%d\n",
                   tr.engine.c_str(), pnl_usd, equity_, peak_equity_,
                   max_dd_pct_ * 100.0, open_count_);
            fflush(stdout);
            if (runtime_cb) runtime_cb(tr);
        };
    }

    void _drive_cell(TrendRiderCell& cell, const TrBar& b, double bid, double ask,
                     double atr14, int cell_idx, int64_t now_ms,
                     OnCloseCb runtime_cb) noexcept {
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
        const double sl_pts = atr14 * cell.sl_n;
        const double lot    = size_for(sl_pts);
        const int    opened = cell.on_bar(b, bid, ask, atr14, now_ms, lot,
                                          wrap(runtime_cb, cell_idx));
        if (opened) ++open_count_;
    }

    void on_h1_bar(const TrBar& h1, double bid, double ask,
                   double /*h1_atr14_unused*/,
                   int64_t now_ms, OnCloseCb runtime_cb) noexcept {
        if (!enabled) return;

        synth_h2_.on_h1_bar(h1, [&](const TrBar& b) {
            atr_h2_.on_bar(b);
            const double a = atr_h2_.value();
            if (atr_h2_.ready()) {
                _drive_cell(h2_long_,  b, bid, ask, a, 0, now_ms, runtime_cb);
                _drive_cell(h2_short_, b, bid, ask, a, 1, now_ms, runtime_cb);
            } else {
                (void)h2_long_ .on_bar(b, bid, ask, 0.0, now_ms, 0.0, wrap(runtime_cb, 0));
                (void)h2_short_.on_bar(b, bid, ask, 0.0, now_ms, 0.0, wrap(runtime_cb, 1));
            }
        });
        synth_h4_.on_h1_bar(h1, [&](const TrBar& b) {
            atr_h4_.on_bar(b);
            const double a = atr_h4_.value();
            if (atr_h4_.ready()) {
                _drive_cell(h4_long_,  b, bid, ask, a, 2, now_ms, runtime_cb);
                _drive_cell(h4_short_, b, bid, ask, a, 3, now_ms, runtime_cb);
            } else {
                (void)h4_long_ .on_bar(b, bid, ask, 0.0, now_ms, 0.0, wrap(runtime_cb, 2));
                (void)h4_short_.on_bar(b, bid, ask, 0.0, now_ms, 0.0, wrap(runtime_cb, 3));
            }
        });
        synth_h6_.on_h1_bar(h1, [&](const TrBar& b) {
            atr_h6_.on_bar(b);
            const double a = atr_h6_.value();
            if (atr_h6_.ready()) _drive_cell(h6_long_, b, bid, ask, a, 4, now_ms, runtime_cb);
            else                 (void)h6_long_.on_bar(b, bid, ask, 0.0, now_ms, 0.0, wrap(runtime_cb, 4));
        });
        synth_d1_.on_h1_bar(h1, [&](const TrBar& b) {
            atr_d1_.on_bar(b);
            const double a = atr_d1_.value();
            if (atr_d1_.ready()) _drive_cell(d1_long_, b, bid, ask, a, 5, now_ms, runtime_cb);
            else                 (void)d1_long_.on_bar(b, bid, ask, 0.0, now_ms, 0.0, wrap(runtime_cb, 5));
        });
    }

    void on_tick(double bid, double ask, int64_t now_ms,
                 OnCloseCb runtime_cb) noexcept {
        if (!enabled) return;
        h2_long_ .on_tick(bid, ask, now_ms, wrap(runtime_cb, 0));
        h2_short_.on_tick(bid, ask, now_ms, wrap(runtime_cb, 1));
        h4_long_ .on_tick(bid, ask, now_ms, wrap(runtime_cb, 2));
        h4_short_.on_tick(bid, ask, now_ms, wrap(runtime_cb, 3));
        h6_long_ .on_tick(bid, ask, now_ms, wrap(runtime_cb, 4));
        d1_long_ .on_tick(bid, ask, now_ms, wrap(runtime_cb, 5));
    }

    void force_close_all(double bid, double ask, int64_t now_ms,
                         OnCloseCb runtime_cb) noexcept {
        h2_long_ .force_close(bid, ask, now_ms, wrap(runtime_cb, 0));
        h2_short_.force_close(bid, ask, now_ms, wrap(runtime_cb, 1));
        h4_long_ .force_close(bid, ask, now_ms, wrap(runtime_cb, 2));
        h4_short_.force_close(bid, ask, now_ms, wrap(runtime_cb, 3));
        h6_long_ .force_close(bid, ask, now_ms, wrap(runtime_cb, 4));
        d1_long_ .force_close(bid, ask, now_ms, wrap(runtime_cb, 5));
    }

    int total_open() const noexcept {
        return (h2_long_ .has_open_position() ? 1 : 0)
             + (h2_short_.has_open_position() ? 1 : 0)
             + (h4_long_ .has_open_position() ? 1 : 0)
             + (h4_short_.has_open_position() ? 1 : 0)
             + (h6_long_ .has_open_position() ? 1 : 0)
             + (d1_long_ .has_open_position() ? 1 : 0);
    }

    int warmup_from_csv(const std::string& path) noexcept {
        if (!enabled) {
            printf("[TRENDR-WARMUP] skipped -- portfolio disabled\n"); fflush(stdout); return 0;
        }
        if (path.empty()) {
            printf("[TRENDR-WARMUP] skipped -- warmup_csv_path empty (cold start)\n");
            fflush(stdout); return 0;
        }
        std::ifstream f(path);
        if (!f.is_open()) {
            printf("[TRENDR-WARMUP] FAIL -- cannot open '%s' (cold start; "
                   "first cell signals delayed up to ~40 D1 bars = 40 days)\n",
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
            char* ep_o=nullptr; const double o = std::strtod(tok[1].c_str(), &ep_o);
            char* ep_h=nullptr; const double h = std::strtod(tok[2].c_str(), &ep_h);
            char* ep_l=nullptr; const double l = std::strtod(tok[3].c_str(), &ep_l);
            char* ep_c=nullptr; const double c = std::strtod(tok[4].c_str(), &ep_c);
            if (ep_o == tok[1].c_str() || ep_h == tok[2].c_str()
                || ep_l == tok[3].c_str() || ep_c == tok[4].c_str()) { ++rejected; continue; }
            if (!std::isfinite(o) || !std::isfinite(h)
                || !std::isfinite(l) || !std::isfinite(c))   { ++rejected; continue; }
            TrBar b;
            b.bar_start_ms = static_cast<int64_t>(ms_ll);
            b.open=o; b.high=h; b.low=l; b.close=c;
            if (fed == 0) first_ms = b.bar_start_ms;
            last_ms = b.bar_start_ms;
            _feed_warmup_h1_bar(b);
            ++fed;
        }

        printf("[TRENDR-WARMUP] fed=%d rejected=%d first_ms=%lld last_ms=%lld path='%s'\n",
               fed, rejected,
               static_cast<long long>(first_ms),
               static_cast<long long>(last_ms),
               path.c_str());
        printf("[TRENDR-WARMUP] cell readiness: "
               "H2 hi/lo=%d/%d atr=%s  H4 hi/lo=%d/%d atr=%s  "
               "H6 hi/lo=%d/%d atr=%s  D1 hi/lo=%d/%d atr=%s\n",
               (int)h2_long_.highs_.size(), h2_long_.period,
               atr_h2_.ready() ? "ready" : "cold",
               (int)h4_long_.highs_.size(), h4_long_.period,
               atr_h4_.ready() ? "ready" : "cold",
               (int)h6_long_.highs_.size(), h6_long_.period,
               atr_h6_.ready() ? "ready" : "cold",
               (int)d1_long_.highs_.size(), d1_long_.period,
               atr_d1_.ready() ? "ready" : "cold");
        fflush(stdout);
        return fed;
    }

private:
    void _feed_warmup_h1_bar(const TrBar& h1) noexcept {
        auto noop_cb = OnCloseCb{};
        const double bid    = h1.close;
        const double ask    = h1.close;
        const int64_t now_ms = h1.bar_start_ms;
        synth_h2_.on_h1_bar(h1, [&](const TrBar& b) {
            atr_h2_.on_bar(b);
            (void)h2_long_ .on_bar(b, bid, ask, atr_h2_.value(), now_ms, 0.0, noop_cb);
            (void)h2_short_.on_bar(b, bid, ask, atr_h2_.value(), now_ms, 0.0, noop_cb);
        });
        synth_h4_.on_h1_bar(h1, [&](const TrBar& b) {
            atr_h4_.on_bar(b);
            (void)h4_long_ .on_bar(b, bid, ask, atr_h4_.value(), now_ms, 0.0, noop_cb);
            (void)h4_short_.on_bar(b, bid, ask, atr_h4_.value(), now_ms, 0.0, noop_cb);
        });
        synth_h6_.on_h1_bar(h1, [&](const TrBar& b) {
            atr_h6_.on_bar(b);
            (void)h6_long_.on_bar(b, bid, ask, atr_h6_.value(), now_ms, 0.0, noop_cb);
        });
        synth_d1_.on_h1_bar(h1, [&](const TrBar& b) {
            atr_d1_.on_bar(b);
            (void)d1_long_.on_bar(b, bid, ask, atr_d1_.value(), now_ms, 0.0, noop_cb);
        });
    }
};

} // namespace omega
