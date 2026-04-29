// =============================================================================
//  TsmomEngine.hpp -- Tier-1 ship: 5 long tsmom cells (H1 / H2 / H4 / H6 / D1)
//  for live shadow paper-trading on XAUUSD.
//
//  Created 2026-04-30. Verdict source:
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
//  master_summary survivor set. Map exactly to the "small quick trades up
//  and down, locks profits" spec.
//
//  SIGNAL (mirrors phase1/signal_discovery/post_cut_revalidate_all.py::sig_tsmom):
//      ret_n = close[t] - close[t - lookback]    # lookback = 20
//      side  = +1 if ret_n > 0    (long fire)
//              -1 if ret_n < 0    (short fire -- not used in Tier-1)
//
//  EXIT (mirrors sim_c):
//      Entry at NEXT bar open after signal close (live: ask for long, bid for short
//      at the moment the signal bar closes -- which is the same instant as the
//      next bar's open boundary).
//      Hard SL: entry +/- 3.0 * ATR14_at_signal (intrabar high/low fill, runtime
//          tick fill via on_tick between bars).
//      Time exit: hold_bars = 12 bars after entry, exit at bar close (no TP).
//
//  COOLDOWN (caveat from SESSION_HANDOFF_2026-04-29_signal_discovery.md):
//      The Python sig_tsmom in post_cut_revalidate_all.py fires every bar
//      where ret_n keeps the same sign -- ~3,484 H1 trades/yr is roughly 4x
//      the master_summary canonical signal frequency because the canonical
//      pipeline carries an additional cooldown / ATR-floor / regime gate.
//      Without a cooldown, deploying live would over-fire and hit
//      max_positions / min_entry_gap throttles, masking actual edge.
//      Per the handoff caveat: cooldown_bars = hold_bars (12) blocks new
//      entries on the same cell until the prior position's full hold window
//      has elapsed. Per-trade edge (WR / PF / avg) is robust across the 4x
//      frequency difference -- this just stops position stacking.
//
//  PORTFOLIO RULES (mirrors C1RetunedPortfolio for consistency):
//      max_concurrent = 5         (one open per cell)
//      risk_pct       = 0.005     (0.5% of equity per trade)
//      start_equity   = $10,000   (paper, shadow only)
//      margin_call    = $1,000
//      max_lot_cap    = 0.05      (tighter than backtest while shadow-validating)
//      block_on_risk_off = true   (skip new entries when g_macroDetector =
//                                  RISK_OFF; existing positions still managed)
//
//  HISTORICAL-BAR WARMUP
//  ---------------------
//  warmup_from_csv() pre-warms every cell's closes_ deque + ATR + bar
//  synthesiser BEFORE live ticks start arriving, by feeding a historical
//  H1 bar CSV through the same pipeline live bars use. Removes the
//  cold-start window (D1 would otherwise need ~21 days of live H1 bars
//  before its closes_ is full). CSV format and generator script are
//  documented inline at warmup_from_csv() below.
//
//  HOW THIS INTEGRATES WITH THE EXISTING RUNTIME
//  ---------------------------------------------
//  Self-contained -- a single header, no edits to existing engine code.
//  Plugs into the runtime via four small additions (mirrors C1RetunedPortfolio):
//
//      1. globals.hpp        : declare static omega::TsmomPortfolio g_tsmom;
//      2. engine_init.hpp    : configure g_tsmom, call g_tsmom.init(), then
//                              g_tsmom.warmup_from_csv(g_tsmom.warmup_csv_path)
//                              inside init_engines(), after the g_c1_retuned
//                              init block.
//      3. tick_gold.hpp      : call g_tsmom.on_h1_bar(...) alongside the
//                              g_c1_retuned.on_h1_bar(...) dispatch.
//                              Call g_tsmom.on_tick(bid, ask, now_ms, ...)
//                              alongside g_c1_retuned.on_tick(...).
//                              Optional: feed macro regime once per bar via
//                              g_tsmom.set_macro_regime(g_macroDetector.regime()).
//      4. omega_config.ini   : new [tsmom] section + 5 [tsmom_<TF>_long]
//                              sub-sections (documentation; engine_init wires
//                              literal values in the C++).
//
//  H1 bars come from g_bars_gold.h1 (existing infrastructure). The portfolio
//  synthesises H2/H4/H6/D1 internally from completed H1 bars (stride 2/4/6/24)
//  and computes its own ATR14 on those synthesised bars. This keeps the
//  integration narrow: the H1-bar-close handler is the only signal the runtime
//  has to feed in.
//
//  SHADOW-MODE DEFAULT
//  -------------------
//  shadow_mode default true. Trades are emitted via the same on_close
//  callback as C1RetunedPortfolio (ca_on_close -> handle_closed_trade), which
//  routes them through the existing shadow CSV pipeline (logs/shadow/
//  omega_shadow.csv). Each TradeRecord is stamped with engine="Tsmom_<TF>_<dir>"
//  and shadow=true so scripts/shadow_analysis.py picks them up unchanged.
//
//  RULES FOLLOWED
//  --------------
//  - Self-contained: no edits to existing engine headers.
//  - Long-only Tier-1: short signals are not generated (Tier-3 will add 5 short
//    cells once shadow ledger validates the long cells).
//  - Shadow-default: shadow_mode=true unless explicitly flipped via
//    g_cfg.mode = "LIVE" (kShadowDefault) in engine_init.hpp.
//  - No core-code modification: this header includes only OmegaTradeLedger.hpp
//    (for TradeRecord) and standard library headers.
//  - Mirrors C1RetunedPortfolio idioms (init() ARMED log, wrap()/_drive_cell()
//    sizing, ca_on_close callback shape) so the Phase 2 shadow tooling slots
//    in unchanged.
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

// =============================================================================
//  Synthesised OHLC bar -- minimal struct used for H1 input and H2/H4/H6/D1
//  internal aggregation. Independent of OHLCBarEngine to keep this header
//  standalone; mirror of C1RetunedPortfolio::C1Bar shape so callers can
//  populate identically.
// =============================================================================
struct TsmomBar {
    int64_t bar_start_ms = 0;
    double  open  = 0.0;
    double  high  = 0.0;
    double  low   = 0.0;
    double  close = 0.0;
};

// =============================================================================
//  TsmomBarSynth -- aggregates N H1 bars into one bigger-TF bar.
//      stride = 2  -> H2
//      stride = 4  -> H4
//      stride = 6  -> H6
//      stride = 24 -> D1
//
//  Calling code feeds completed H1 bars in via on_h1_bar(); the synthesiser
//  emits a completed TsmomBar via the user-supplied callback whenever stride
//  bars have accumulated. Mirrors C1BarSynth so behaviour is identical.
// =============================================================================
struct TsmomBarSynth {
    int      stride       = 1;
    int      accum_count_ = 0;
    TsmomBar cur_{};

    using EmitCallback = std::function<void(const TsmomBar&)>;

    void on_h1_bar(const TsmomBar& h1, EmitCallback emit) noexcept {
        if (accum_count_ == 0) {
            cur_              = h1;
            cur_.bar_start_ms = h1.bar_start_ms;  // start of first H1 bar
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

// =============================================================================
//  TsmomATR14 -- Wilder ATR14 over completed bars. Self-contained -- no
//  shared state with OHLCBarEngine. Bootstraps with a simple SMA over the
//  first ATR_P bars, then switches to Wilder smoothing.
// =============================================================================
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

        if (n_ < ATR_P) {
            atr_ = (atr_ * n_ + tr) / (n_ + 1);
            n_++;
        } else {
            atr_ = (atr_ * (ATR_P - 1) + tr) / ATR_P;
        }

        prev_close_     = b.close;
        has_prev_close_ = true;
    }

    double value() const noexcept { return atr_; }
};

// =============================================================================
//  TsmomCell -- single tsmom cell: one timeframe, one direction (Tier-1 long
//  only). lookback=20 close-to-close return, hard SL = 3*ATR14, time exit at
//  hold_bars=12, no TP, cooldown=hold_bars to prevent stacking.
//
//  Bar-driven: on_bar() runs at every completed parent bar (for H1: every
//  H1 close; for H2: every synthesised H2 close; etc.). on_tick() runs every
//  XAUUSD tick to fire intrabar SL hits between bars (mirror of C1Retuned
//  cells). force_close() is the shutdown path.
// =============================================================================
struct TsmomCell {
    using OnCloseCb = std::function<void(const TradeRecord&)>;

    // ---- Configuration (set once at init; immutable during run) -------------
    bool   shadow_mode    = true;
    bool   enabled        = true;
    int    lookback       = 20;        // close-to-close return window
    int    hold_bars      = 12;        // time exit horizon
    int    cooldown_bars  = 12;        // == hold_bars (anti-stacking)
    double hard_sl_atr    = 3.0;       // hard SL = hard_sl_atr * ATR14_at_signal
    double max_spread_pt  = 1.5;       // entry blocked if (ask-bid) > this

    int    direction      = 1;          // +1 long, -1 short (Tier-1 = +1)
    std::string timeframe = "H1";       // diagnostic only
    std::string symbol    = "XAUUSD";
    std::string cell_id   = "Tsmom_H1_long";

    // ---- Rolling closes window for diff(lookback) ---------------------------
    std::deque<double> closes_;

    // ---- Position state (single open position per cell) ---------------------
    bool    pos_active_     = false;
    double  pos_entry_      = 0.0;
    double  pos_sl_         = 0.0;
    double  pos_size_       = 0.0;
    double  pos_atr_        = 0.0;
    int64_t pos_entry_ms_   = 0;
    int     pos_bars_held_  = 0;
    double  pos_mfe_        = 0.0;     // signed best move (>= 0 once profitable)
    double  pos_mae_        = 0.0;     // signed worst move (<= 0 if adverse)
    double  pos_spread_at_  = 0.0;     // bid-ask at entry (pts)
    int     trade_id_       = 0;

    // ---- Diagnostics --------------------------------------------------------
    int    bar_count_      = 0;
    int    cooldown_left_  = 0;        // bars remaining in post-exit cooldown

    bool has_open_position() const noexcept { return pos_active_; }

    // -------------------------------------------------------------------------
    //  on_bar -- called once per completed parent bar.
    //
    //  Order of operations:
    //    1. If a position is open: increment hold counter, check intrabar SL
    //       (using bar high/low), check time exit. Return -- never process a
    //       new signal while a position is open.
    //    2. Update rolling closes window.
    //    3. If cooldown active, decrement and return (no signal processing).
    //    4. Spread check, ATR availability check, lookback warmup check.
    //    5. Compute ret_n = closes_.back() - closes_[end-1-lookback].
    //    6. If sign matches `direction`, open a position at ask (long) or
    //       bid (short).
    //
    //  Returns 1 if a NEW position opened this bar, 0 otherwise.
    // -------------------------------------------------------------------------
    int on_bar(const TsmomBar& b, double bid, double ask, double atr14_at_signal,
               int64_t now_ms, double size_lot, OnCloseCb on_close) noexcept {
        ++bar_count_;

        // ----- 1. Manage existing position --------------------------------
        if (pos_active_) {
            ++pos_bars_held_;

            // Track MFE / MAE on bar high/low (signed, in price points,
            // direction-aware so MFE >= 0 and MAE <= 0 for typical paths).
            const double max_move_this_bar = direction == 1
                ? (b.high - pos_entry_)
                : (pos_entry_ - b.low);
            const double min_move_this_bar = direction == 1
                ? (b.low  - pos_entry_)
                : (pos_entry_ - b.high);
            if (max_move_this_bar > pos_mfe_) pos_mfe_ = max_move_this_bar;
            if (min_move_this_bar < pos_mae_) pos_mae_ = min_move_this_bar;

            // Hard SL hit intrabar (long: bar low pierced SL; short: bar high)
            const bool sl_hit = direction == 1
                ? (b.low  <= pos_sl_)
                : (b.high >= pos_sl_);
            if (sl_hit) {
                _close(pos_sl_, "SL_HIT", now_ms, on_close);
                cooldown_left_ = cooldown_bars;
                return 0;
            }

            // Time exit at hold_bars (matches sim_c: end = i+1+hold_bars,
            // exit at close of end-1 = i+hold_bars, i.e. after `hold_bars`
            // completed bars of holding).
            if (pos_bars_held_ >= hold_bars) {
                _close(b.close, "TIME_EXIT", now_ms, on_close);
                cooldown_left_ = cooldown_bars;
                return 0;
            }
            return 0;
        }

        // ----- 2. Update rolling closes window ----------------------------
        closes_.push_back(b.close);
        const std::size_t cap = static_cast<std::size_t>(lookback) + 1;
        while (closes_.size() > cap) closes_.pop_front();

        // ----- 3. Decrement cooldown -------------------------------------
        if (cooldown_left_ > 0) {
            --cooldown_left_;
            return 0;
        }

        // ----- 4. Pre-fire gates -----------------------------------------
        if (!enabled) return 0;
        if ((int)closes_.size() < lookback + 1) return 0;
        if (!std::isfinite(atr14_at_signal) || atr14_at_signal <= 0.0) return 0;
        const double spread_pt = ask - bid;
        if (!std::isfinite(spread_pt) || spread_pt < 0.0) return 0;
        if (spread_pt > max_spread_pt) return 0;
        if (size_lot <= 0.0) return 0;

        // ----- 5. tsmom signal: ret_n = current close - close `lookback`
        //         bars ago. Mirrors df["close_mid"].diff(20).
        const double cur     = closes_.back();
        const double earlier = closes_[closes_.size() - 1 - lookback];
        const double ret_n   = cur - earlier;

        const int sig_dir =
            (ret_n > 0.0) ? +1 :
            (ret_n < 0.0) ? -1 : 0;
        if (sig_dir == 0)             return 0;
        if (sig_dir != direction)     return 0;   // Tier-1: long-only

        // ----- 6. Open the position --------------------------------------
        // Live-runtime equivalent of "entry at next bar open": at the moment
        // the signal bar closes, the next bar's open is the very next tick,
        // so we fill at ask (long) / bid (short) of the bar-close tick.
        const double entry_px = direction == 1 ? ask : bid;
        const double sl_pts   = atr14_at_signal * hard_sl_atr;
        const double sl_px    = entry_px - direction * sl_pts;

        pos_active_     = true;
        pos_entry_      = entry_px;
        pos_sl_         = sl_px;
        pos_size_       = size_lot;
        pos_atr_        = atr14_at_signal;
        pos_entry_ms_   = now_ms;
        pos_bars_held_  = 0;
        pos_mfe_        = 0.0;
        pos_mae_        = 0.0;
        pos_spread_at_  = spread_pt;
        ++trade_id_;

        printf("[%s] ENTRY %s @ %.2f sl=%.2f size=%.4f atr=%.3f"
               " ret_n=%.3f spread=%.2f%s\n",
               cell_id.c_str(),
               direction == 1 ? "LONG" : "SHORT",
               entry_px, sl_px, pos_size_, atr14_at_signal,
               ret_n, spread_pt,
               shadow_mode ? " [SHADOW]" : "");
        fflush(stdout);
        return 1;
    }

    // -------------------------------------------------------------------------
    //  on_tick -- intrabar position management. Fires hard SL between bars
    //  (on_bar already covers SL hits at bar-close granularity using the
    //  bar high/low, but a long-tail tick stream needs a tick-level fill so
    //  TradeRecord.exitTs is accurate to the second, not the bar boundary).
    //  Mirrors C1RetunedPortfolio::C1*Cell::on_tick.
    // -------------------------------------------------------------------------
    void on_tick(double bid, double ask, int64_t now_ms,
                 OnCloseCb on_close) noexcept {
        if (!pos_active_) return;

        // Signed favourable move (positive = profitable, negative = adverse).
        // For a LONG: best favourable price is the bid we could close at.
        // For a SHORT: best favourable price is the ask we could cover at.
        const double signed_move = direction == 1
            ? (bid - pos_entry_)
            : (pos_entry_ - ask);
        if (signed_move > pos_mfe_) pos_mfe_ = signed_move;
        if (signed_move < pos_mae_) pos_mae_ = signed_move;

        // Tick-level SL fill (mirror of C1RetunedPortfolio cells).
        if (direction == 1) {
            if (bid <= pos_sl_) {
                _close(bid, "SL_HIT", now_ms, on_close);
                cooldown_left_ = cooldown_bars;
            }
        } else {
            if (ask >= pos_sl_) {
                _close(ask, "SL_HIT", now_ms, on_close);
                cooldown_left_ = cooldown_bars;
            }
        }
    }

    // -------------------------------------------------------------------------
    //  force_close -- shutdown / weekend-gap / circuit-breaker exit.
    // -------------------------------------------------------------------------
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
        // pnl_pts is signed: gross price-point pnl scaled by lots.
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
            tr.tp            = 0.0;            // no TP -- tsmom uses time exit
            tr.size          = pos_size_;
            tr.pnl           = pnl_pts;        // raw pts*lots; handle_closed_trade
                                               // applies tick_value_multiplier
            tr.mfe           = pos_mfe_ * pos_size_;
            tr.mae           = pos_mae_ * pos_size_;
            tr.entryTs       = pos_entry_ms_ / 1000;
            tr.exitTs        = now_ms / 1000;
            tr.exitReason    = reason;
            tr.regime        = "TSMOM";
            tr.atr_at_entry  = pos_atr_;
            tr.spreadAtEntry = pos_spread_at_;
            tr.shadow        = shadow_mode;
            on_close(tr);
        }

        pos_active_     = false;
        pos_entry_      = 0.0;
        pos_sl_         = 0.0;
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
//  TsmomPortfolio -- orchestrator. Owns 5 long cells (H1 / H2 / H4 / H6 / D1),
//  enforces max_concurrent and 0.5%-risk sizing, exposes a single H1 bar
//  dispatch entry point and a single tick dispatch entry point. Synthesises
//  H2/H4/H6/D1 bars from the H1 stream so the runtime only has to feed in
//  one bar source.
//
//  USAGE
//  -----
//      // globals.hpp:
//      static omega::TsmomPortfolio g_tsmom;
//
//      // engine_init.hpp (after g_c1_retuned init block):
//      g_tsmom.shadow_mode    = kShadowDefault;   // follows g_cfg.mode
//      g_tsmom.enabled        = true;
//      g_tsmom.max_concurrent = 5;
//      g_tsmom.risk_pct       = 0.005;
//      g_tsmom.start_equity   = 10000.0;
//      g_tsmom.margin_call    = 1000.0;
//      g_tsmom.max_lot_cap    = 0.05;
//      g_tsmom.block_on_risk_off = true;
//      g_tsmom.warmup_csv_path   = "phase1/signal_discovery/tsmom_warmup_H1.csv";
//      g_tsmom.init();
//      g_tsmom.warmup_from_csv(g_tsmom.warmup_csv_path);
//
//      // tick_gold.hpp inside the H1-close block (after g_bars_gold.h1.add_bar):
//      omega::TsmomBar h1{};
//      h1.bar_start_ms = s_bar_h1_ms;
//      h1.open  = s_cur_h1.open;
//      h1.high  = s_cur_h1.high;
//      h1.low   = s_cur_h1.low;
//      h1.close = s_cur_h1.close;
//      g_tsmom.set_macro_regime(g_macroDetector.regime());  // optional
//      g_tsmom.on_h1_bar(h1, bid, ask,
//          g_bars_gold.h1.ind.atr14.load(std::memory_order_relaxed),
//          now_ms_g, ca_on_close);
//
//      // tick_gold.hpp on every tick (alongside g_c1_retuned.on_tick):
//      g_tsmom.on_tick(bid, ask, now_ms_g, ca_on_close);
//
//  The orchestrator wraps the on_close callback so it can update its own
//  paper equity / drawdown state before forwarding to the runtime callback.
// =============================================================================
struct TsmomPortfolio {
    using OnCloseCb = std::function<void(const TradeRecord&)>;

    // ---- Configuration ------------------------------------------------------
    bool   enabled           = true;
    bool   shadow_mode       = true;
    int    max_concurrent    = 5;
    double risk_pct          = 0.005;
    double start_equity      = 10000.0;
    double margin_call       = 1000.0;
    double max_lot_cap       = 0.05;
    bool   block_on_risk_off = true;

    // Path to a CSV of historical H1 bars used to pre-warm closes_/synthesisers/
    // ATRs at startup. Empty = no warmup (cold start). Set in engine_init.hpp
    // and consumed by warmup_from_csv() after init().
    //
    // Expected CSV format (one row per H1 bar, comma-separated, optional
    // header which is skipped if its first field is not a valid integer):
    //   bar_start_ms,open,high,low,close
    //
    // Source: phase1/signal_discovery/bars_H1.parquet, exported via
    //   phase1/signal_discovery/export_tsmom_warmup.py
    std::string warmup_csv_path = "";

    // ---- Cells (long-only Tier-1) -------------------------------------------
    TsmomCell h1_long_;
    TsmomCell h2_long_;
    TsmomCell h4_long_;
    TsmomCell h6_long_;
    TsmomCell d1_long_;

    // ---- Internal H2/H4/H6/D1 bar synthesis ---------------------------------
    TsmomBarSynth synth_h2_;     // stride 2
    TsmomBarSynth synth_h4_;     // stride 4
    TsmomBarSynth synth_h6_;     // stride 6
    TsmomBarSynth synth_d1_;     // stride 24

    // ATR14 per-timeframe. atr_h1_ is internal so we don't depend on
    // g_bars_gold.h1.ind.atr14 being warm at startup -- the runtime ATR is
    // preferred when it's > 0 (matches g_bars_gold behaviour after its own
    // warmup loads), but if it's still cold (e.g. first H1 bar after a
    // restart with no g_bars_gold state on disk) we fall back to atr_h1_.
    // H2/H4/H6/D1 self-compute because g_bars_gold has no synthesised feeds
    // at those strides.
    TsmomATR14 atr_h1_;
    TsmomATR14 atr_h2_;
    TsmomATR14 atr_h4_;
    TsmomATR14 atr_h6_;
    TsmomATR14 atr_d1_;

    // ---- Paper-equity / drawdown tracking -----------------------------------
    double equity_                 = 10000.0;
    double peak_equity_            = 10000.0;
    double max_dd_pct_             = 0.0;
    int    open_count_             = 0;
    int    blocked_max_concurrent_ = 0;
    int    blocked_risk_off_       = 0;

    // ---- Macro regime feed (optional) ---------------------------------------
    std::string macro_regime_      = "NEUTRAL";
    void set_macro_regime(const std::string& r) noexcept { macro_regime_ = r; }

    // -------------------------------------------------------------------------
    //  init -- stamp cell ids / directions / shadow_mode and emit the
    //  ARMED log line. Call once after configuring the portfolio in
    //  engine_init.hpp.
    // -------------------------------------------------------------------------
    void init() noexcept {
        h1_long_.symbol = "XAUUSD"; h1_long_.cell_id = "Tsmom_H1_long"; h1_long_.timeframe = "H1";
        h2_long_.symbol = "XAUUSD"; h2_long_.cell_id = "Tsmom_H2_long"; h2_long_.timeframe = "H2";
        h4_long_.symbol = "XAUUSD"; h4_long_.cell_id = "Tsmom_H4_long"; h4_long_.timeframe = "H4";
        h6_long_.symbol = "XAUUSD"; h6_long_.cell_id = "Tsmom_H6_long"; h6_long_.timeframe = "H6";
        d1_long_.symbol = "XAUUSD"; d1_long_.cell_id = "Tsmom_D1_long"; d1_long_.timeframe = "D1";

        h1_long_.direction = 1;
        h2_long_.direction = 1;
        h4_long_.direction = 1;
        h6_long_.direction = 1;
        d1_long_.direction = 1;

        // Propagate shadow_mode to all cells (engine_init flips this once per
        // run via kShadowDefault before calling init()).
        h1_long_.shadow_mode = shadow_mode;
        h2_long_.shadow_mode = shadow_mode;
        h4_long_.shadow_mode = shadow_mode;
        h6_long_.shadow_mode = shadow_mode;
        d1_long_.shadow_mode = shadow_mode;

        synth_h2_.stride =  2;
        synth_h4_.stride =  4;
        synth_h6_.stride =  6;
        synth_d1_.stride = 24;

        equity_      = start_equity;
        peak_equity_ = start_equity;

        printf("[TSMOM] TsmomPortfolio ARMED (shadow_mode=%s, enabled=%s) "
               "cells=H1,H2,H4,H6,D1 long lookback=%d hold_bars=%d "
               "sl=%.1f*ATR cooldown=%d "
               "risk_pct=%.4f max_lot_cap=%.3f max_concurrent=%d "
               "block_on_risk_off=%s equity=$%.2f\n",
               shadow_mode ? "true" : "false",
               enabled     ? "true" : "false",
               h1_long_.lookback,
               h1_long_.hold_bars,
               h1_long_.hard_sl_atr,
               h1_long_.cooldown_bars,
               risk_pct, max_lot_cap, max_concurrent,
               block_on_risk_off ? "true" : "false",
               equity_);
        fflush(stdout);

        printf("[Tsmom_H1_long] ARMED (shadow_mode=%s, lookback=%d, hold=%d, sl=%.1f*atr)\n",
               h1_long_.shadow_mode ? "true" : "false",
               h1_long_.lookback, h1_long_.hold_bars, h1_long_.hard_sl_atr);
        printf("[Tsmom_H2_long] ARMED (shadow_mode=%s, lookback=%d, hold=%d, sl=%.1f*atr)\n",
               h2_long_.shadow_mode ? "true" : "false",
               h2_long_.lookback, h2_long_.hold_bars, h2_long_.hard_sl_atr);
        printf("[Tsmom_H4_long] ARMED (shadow_mode=%s, lookback=%d, hold=%d, sl=%.1f*atr)\n",
               h4_long_.shadow_mode ? "true" : "false",
               h4_long_.lookback, h4_long_.hold_bars, h4_long_.hard_sl_atr);
        printf("[Tsmom_H6_long] ARMED (shadow_mode=%s, lookback=%d, hold=%d, sl=%.1f*atr)\n",
               h6_long_.shadow_mode ? "true" : "false",
               h6_long_.lookback, h6_long_.hold_bars, h6_long_.hard_sl_atr);
        printf("[Tsmom_D1_long] ARMED (shadow_mode=%s, lookback=%d, hold=%d, sl=%.1f*atr)\n",
               d1_long_.shadow_mode ? "true" : "false",
               d1_long_.lookback, d1_long_.hold_bars, d1_long_.hard_sl_atr);
        fflush(stdout);
    }

    // -------------------------------------------------------------------------
    //  Position-sizing helper. Mirror of C1RetunedPortfolio::size_for so
    //  shadow ledger PnL scales identically across both portfolios.
    //
    //  size = (equity * risk_pct) / (sl_pts * 100)
    //    -- 100 because lot=1.0 is 100oz on XAUUSD, $1/pt/oz.
    //  Floored at 0.01 (broker minimum) and capped at max_lot_cap.
    // -------------------------------------------------------------------------
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

    // Wrap the runtime's on_close callback so we can update equity / DD /
    // open_count before forwarding. cell_idx unused in Tier-1 (no cluster
    // tracking yet) but kept for parity with C1RetunedPortfolio::wrap and
    // future cluster-day analytics.
    OnCloseCb wrap(OnCloseCb runtime_cb, int /*cell_idx*/) {
        return [this, runtime_cb](const TradeRecord& tr) {
            const double pnl_usd = tr.pnl * 100.0;   // pts*lots * 100 = USD on XAUUSD
            equity_ += pnl_usd;
            if (equity_ > peak_equity_) peak_equity_ = equity_;
            const double dd = peak_equity_ > 0.0
                ? (equity_ - peak_equity_) / peak_equity_
                : 0.0;
            if (dd < max_dd_pct_) max_dd_pct_ = dd;
            open_count_ = std::max(0, open_count_ - 1);

            printf("[TSMOM-CLOSE] cell=%s pnl=$%.2f equity=$%.2f peak=$%.2f"
                   " dd=%.2f%% open=%d\n",
                   tr.engine.c_str(), pnl_usd, equity_, peak_equity_,
                   max_dd_pct_ * 100.0, open_count_);
            fflush(stdout);

            if (runtime_cb) runtime_cb(tr);
        };
    }

    // Drive a single cell on its parent timeframe's bar close. Encapsulates
    // the manage-vs-open branching, sizing, and concurrency-blocked logging
    // so the on_h1_bar() body stays compact.
    void _drive_cell(TsmomCell& cell, const TsmomBar& b, double bid, double ask,
                     double atr14, int cell_idx, int64_t now_ms,
                     OnCloseCb runtime_cb) noexcept {
        if (cell.has_open_position()) {
            (void)cell.on_bar(b, bid, ask, atr14, now_ms, 0.0,
                              wrap(runtime_cb, cell_idx));
            return;
        }
        if (!can_open()) {
            if (block_on_risk_off && macro_regime_ == "RISK_OFF") {
                ++blocked_risk_off_;
            } else {
                ++blocked_max_concurrent_;
            }
            // Still tick the bar through cell.on_bar to update its closes_
            // window / cooldown counter -- otherwise a long RISK_OFF window
            // leaves the cell with stale closes when the regime clears.
            (void)cell.on_bar(b, bid, ask, atr14, now_ms, 0.0,
                              wrap(runtime_cb, cell_idx));
            return;
        }
        const double sl_pts = atr14 * cell.hard_sl_atr;
        const double lot    = size_for(sl_pts);
        const int    opened = cell.on_bar(b, bid, ask, atr14, now_ms, lot,
                                          wrap(runtime_cb, cell_idx));
        if (opened) ++open_count_;
    }

    // -------------------------------------------------------------------------
    //  on_h1_bar -- the SINGLE integration point for completed bars. Drives
    //  the H1 cell directly and synthesises H2 / H4 / H6 / D1 bars from the
    //  H1 stream for the other 4 cells.
    //
    //  H1 ATR sourcing: prefer the runtime-supplied h1_atr14 when > 0 (matches
    //  g_bars_gold.h1.ind.atr14 so all gold engines size identically). When
    //  cold (first bar after a restart with no g_bars_gold state on disk),
    //  fall back to the internal atr_h1_ which has been warmed by
    //  warmup_from_csv() and stays hot via the on_bar call below.
    //  H2/H4/H6/D1 always self-compute via atr_h2_/atr_h4_/atr_h6_/atr_d1_.
    // -------------------------------------------------------------------------
    void on_h1_bar(const TsmomBar& h1, double bid, double ask, double h1_atr14,
                   int64_t now_ms, OnCloseCb runtime_cb) noexcept {
        if (!enabled) return;

        // Keep internal H1 ATR hot regardless of runtime ATR state.
        atr_h1_.on_bar(h1);

        const double eff_h1_atr =
            (std::isfinite(h1_atr14) && h1_atr14 > 0.0) ? h1_atr14
                                                        : atr_h1_.value();
        if (eff_h1_atr > 0.0) {
            _drive_cell(h1_long_, h1, bid, ask, eff_h1_atr, 0, now_ms, runtime_cb);
        } else {
            // Still cold: tick the cell so its closes_ keeps filling.
            (void)h1_long_.on_bar(h1, bid, ask, 0.0, now_ms, 0.0,
                                  wrap(runtime_cb, 0));
        }

        // ---- H2 cell: synth + self-ATR ----
        synth_h2_.on_h1_bar(h1, [&](const TsmomBar& b) {
            atr_h2_.on_bar(b);
            if (atr_h2_.ready()) {
                _drive_cell(h2_long_, b, bid, ask, atr_h2_.value(), 1,
                            now_ms, runtime_cb);
            } else {
                (void)h2_long_.on_bar(b, bid, ask, 0.0, now_ms, 0.0,
                                      wrap(runtime_cb, 1));
            }
        });

        // ---- H4 cell: synth + self-ATR ----
        synth_h4_.on_h1_bar(h1, [&](const TsmomBar& b) {
            atr_h4_.on_bar(b);
            if (atr_h4_.ready()) {
                _drive_cell(h4_long_, b, bid, ask, atr_h4_.value(), 2,
                            now_ms, runtime_cb);
            } else {
                (void)h4_long_.on_bar(b, bid, ask, 0.0, now_ms, 0.0,
                                      wrap(runtime_cb, 2));
            }
        });

        // ---- H6 cell: synth + self-ATR ----
        synth_h6_.on_h1_bar(h1, [&](const TsmomBar& b) {
            atr_h6_.on_bar(b);
            if (atr_h6_.ready()) {
                _drive_cell(h6_long_, b, bid, ask, atr_h6_.value(), 3,
                            now_ms, runtime_cb);
            } else {
                (void)h6_long_.on_bar(b, bid, ask, 0.0, now_ms, 0.0,
                                      wrap(runtime_cb, 3));
            }
        });

        // ---- D1 cell: synth (stride=24) + self-ATR ----
        synth_d1_.on_h1_bar(h1, [&](const TsmomBar& b) {
            atr_d1_.on_bar(b);
            if (atr_d1_.ready()) {
                _drive_cell(d1_long_, b, bid, ask, atr_d1_.value(), 4,
                            now_ms, runtime_cb);
            } else {
                (void)d1_long_.on_bar(b, bid, ask, 0.0, now_ms, 0.0,
                                      wrap(runtime_cb, 4));
            }
        });
    }

    // -------------------------------------------------------------------------
    //  on_tick -- tick-level position management for all 5 cells. Mirrors
    //  C1RetunedPortfolio::on_tick.
    // -------------------------------------------------------------------------
    void on_tick(double bid, double ask, int64_t now_ms,
                 OnCloseCb runtime_cb) noexcept {
        if (!enabled) return;
        h1_long_.on_tick(bid, ask, now_ms, wrap(runtime_cb, 0));
        h2_long_.on_tick(bid, ask, now_ms, wrap(runtime_cb, 1));
        h4_long_.on_tick(bid, ask, now_ms, wrap(runtime_cb, 2));
        h6_long_.on_tick(bid, ask, now_ms, wrap(runtime_cb, 3));
        d1_long_.on_tick(bid, ask, now_ms, wrap(runtime_cb, 4));
    }

    // -------------------------------------------------------------------------
    //  force_close_all -- shutdown / weekend-gap dispatch. Mirrors
    //  C1RetunedPortfolio::force_close_all.
    // -------------------------------------------------------------------------
    void force_close_all(double bid, double ask, int64_t now_ms,
                         OnCloseCb runtime_cb) noexcept {
        h1_long_.force_close(bid, ask, now_ms, wrap(runtime_cb, 0));
        h2_long_.force_close(bid, ask, now_ms, wrap(runtime_cb, 1));
        h4_long_.force_close(bid, ask, now_ms, wrap(runtime_cb, 2));
        h6_long_.force_close(bid, ask, now_ms, wrap(runtime_cb, 3));
        d1_long_.force_close(bid, ask, now_ms, wrap(runtime_cb, 4));
    }

    int total_open() const noexcept {
        return (h1_long_.has_open_position() ? 1 : 0)
             + (h2_long_.has_open_position() ? 1 : 0)
             + (h4_long_.has_open_position() ? 1 : 0)
             + (h6_long_.has_open_position() ? 1 : 0)
             + (d1_long_.has_open_position() ? 1 : 0);
    }

    // Diagnostic accessor for monitors / dashboards.
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
            open_count_, blocked_max_concurrent_, blocked_risk_off_,
            macro_regime_,
        };
    }

    // -------------------------------------------------------------------------
    //  warmup_from_csv -- pre-warm closes_ deques, synthesisers, and ATRs
    //  from a historical H1 bar CSV before live ticks start arriving.
    //
    //  Removes the cold-start window: when the first live H1 bar fires, every
    //  cell is fully warm and signal evaluation runs on the same feature
    //  state the post-cut backtest used.
    //
    //  CSV format (one row per H1 bar):
    //      bar_start_ms,open,high,low,close
    //  - First line: optional header (skipped if first field is not a valid
    //    integer).
    //  - Blank lines and lines starting with '#' are skipped.
    //  - Bars must be in ascending bar_start_ms order; the parser does not
    //    re-sort.
    //
    //  Source: phase1/signal_discovery/bars_H1.parquet, exported via
    //  phase1/signal_discovery/export_tsmom_warmup.py.
    //
    //  Behaviour:
    //  - Each historical bar is fed through the SAME synthesiser / indicator
    //    path live bars use, with size_lot=0.0 and a no-op on_close callback.
    //    size=0 short-circuits the entry path before any TradeRecord is
    //    built, so warmup never produces fake trades or perturbs equity_ /
    //    peak_equity_ / max_dd_pct_.
    //  - cooldown_left_ stays at 0 throughout (no positions ever close
    //    during warmup so no _close() ever assigns it).
    //  - macro_regime_ stays at its current value (caller may set it once
    //    before warmup if a historical regime feed is desired; otherwise
    //    NEUTRAL).
    //  - Runs only if `enabled` is true and `path` is non-empty.
    //
    //  Returns the number of bars successfully fed.
    // -------------------------------------------------------------------------
    int warmup_from_csv(const std::string& path) noexcept {
        if (!enabled) {
            printf("[TSMOM-WARMUP] skipped -- portfolio disabled\n");
            fflush(stdout);
            return 0;
        }
        if (path.empty()) {
            printf("[TSMOM-WARMUP] skipped -- warmup_csv_path empty (cold start)\n");
            fflush(stdout);
            return 0;
        }
        std::ifstream f(path);
        if (!f.is_open()) {
            printf("[TSMOM-WARMUP] FAIL -- cannot open '%s' (cold start; "
                   "first cell signals may be delayed up to ~21 days for D1)\n",
                   path.c_str());
            fflush(stdout);
            return 0;
        }

        int     fed       = 0;
        int     rejected  = 0;
        int64_t first_ms  = 0;
        int64_t last_ms   = 0;
        std::string line;
        while (std::getline(f, line)) {
            // Strip trailing CR (Windows-style line endings).
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty())                  continue;
            if (line[0] == '#')                continue;

            // Tokenise on commas. Expect 5 fields: ms, o, h, l, c.
            std::array<std::string, 5> tok;
            int idx = 0;
            std::stringstream ss(line);
            std::string field;
            while (idx < 5 && std::getline(ss, field, ',')) {
                tok[idx++] = field;
            }
            if (idx < 5) { ++rejected; continue; }

            // First field must parse as an integer. Header rows ("bar_start_ms")
            // fail this and get silently skipped.
            char*   endp     = nullptr;
            const long long ms_ll = std::strtoll(tok[0].c_str(), &endp, 10);
            if (endp == tok[0].c_str() || *endp != '\0') {
                ++rejected;
                continue;
            }

            char*  ep_o = nullptr; const double o = std::strtod(tok[1].c_str(), &ep_o);
            char*  ep_h = nullptr; const double h = std::strtod(tok[2].c_str(), &ep_h);
            char*  ep_l = nullptr; const double l = std::strtod(tok[3].c_str(), &ep_l);
            char*  ep_c = nullptr; const double c = std::strtod(tok[4].c_str(), &ep_c);
            if (ep_o == tok[1].c_str() || ep_h == tok[2].c_str()
                || ep_l == tok[3].c_str() || ep_c == tok[4].c_str()) {
                ++rejected;
                continue;
            }
            if (!std::isfinite(o) || !std::isfinite(h)
                || !std::isfinite(l) || !std::isfinite(c)) {
                ++rejected;
                continue;
            }

            TsmomBar b;
            b.bar_start_ms = static_cast<int64_t>(ms_ll);
            b.open  = o;
            b.high  = h;
            b.low   = l;
            b.close = c;

            if (fed == 0) first_ms = b.bar_start_ms;
            last_ms = b.bar_start_ms;

            _feed_warmup_h1_bar(b);
            ++fed;
        }

        printf("[TSMOM-WARMUP] fed=%d rejected=%d "
               "first_ms=%lld last_ms=%lld path='%s'\n",
               fed, rejected,
               static_cast<long long>(first_ms),
               static_cast<long long>(last_ms),
               path.c_str());
        printf("[TSMOM-WARMUP] cell readiness: "
               "H1 closes=%d/%d  H2 closes=%d/%d atr=%s  "
               "H4 closes=%d/%d atr=%s  H6 closes=%d/%d atr=%s  "
               "D1 closes=%d/%d atr=%s\n",
               static_cast<int>(h1_long_.closes_.size()), h1_long_.lookback + 1,
               static_cast<int>(h2_long_.closes_.size()), h2_long_.lookback + 1,
               atr_h2_.ready() ? "ready" : "cold",
               static_cast<int>(h4_long_.closes_.size()), h4_long_.lookback + 1,
               atr_h4_.ready() ? "ready" : "cold",
               static_cast<int>(h6_long_.closes_.size()), h6_long_.lookback + 1,
               atr_h6_.ready() ? "ready" : "cold",
               static_cast<int>(d1_long_.closes_.size()), d1_long_.lookback + 1,
               atr_d1_.ready() ? "ready" : "cold");
        fflush(stdout);
        return fed;
    }

private:
    // Feed a single historical H1 bar through the same path live bars use,
    // but with size_lot=0.0 so the entry branch in TsmomCell::on_bar
    // short-circuits before any TradeRecord is built. closes_, ATRs, and
    // synthesisers all update normally.
    //
    // We use the bar close as both bid and ask (zero historical spread).
    // The spread gate inside TsmomCell::on_bar would block on size_lot=0
    // anyway (size check fires before any signal evaluation), but using
    // close-as-mid means MFE/MAE tracking inside any never-fired path
    // would still be sane.
    void _feed_warmup_h1_bar(const TsmomBar& h1) noexcept {
        // No-op callback -- warmup must NEVER produce a TradeRecord. Belt-and-
        // braces: size_lot=0.0 short-circuits the entry path; even if the cell
        // logic ever changed to call _close from a manage-only path, this
        // callback drops the record on the floor.
        auto noop_cb = OnCloseCb{};

        const double bid = h1.close;
        const double ask = h1.close;
        const int64_t now_ms = h1.bar_start_ms;

        // Update internal H1 ATR exactly like on_h1_bar would.
        atr_h1_.on_bar(h1);

        // H1 cell: pass size_lot=0 so even if signals fired they could not
        // open a position. Pass the internal H1 ATR for consistency -- but
        // since size_lot=0, ATR is unused.
        (void)h1_long_.on_bar(h1, bid, ask, atr_h1_.value(), now_ms, 0.0, noop_cb);

        // Synthesisers: callbacks update each TF's ATR + closes_ via the
        // cell's on_bar with size_lot=0.
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
