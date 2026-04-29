// =============================================================================
//  C1RetunedPortfolio.hpp -- C++ implementation of the C1_retuned portfolio
//  for live shadow paper-trading.
//
//  Created 2026-04-29. Verdict source: phase2/donchian_postregime/CHOSEN.md.
//  Backtest baseline (2024-03..2026-04 corpus):
//      return +74.12% / max DD -5.85% / PF 1.486 / Sharpe 2.651 / WR 55.2%
//  Walk-forward TRAIN/VALIDATE/TEST all PASS. Post-regime PF 1.334 -> 1.630.
//  Detail: phase2/optionD/walkforward_C_report.txt + C1_C2_summary.txt.
//
//  Portfolio composition (long-only, XAUUSD only):
//      Cell 1: Donchian H1 long retuned  (period=20, sl=3.0 ATR, tp=5.0 ATR)
//      Cell 2: Bollinger H2 long         (touch lower band, exit on midline)
//      Cell 3: Bollinger H4 long         (touch lower band, exit on midline)
//      Cell 4: Bollinger H6 long         (touch lower band, exit on midline)
//
//  Portfolio rules:
//      max_concurrent = 4              (enforced across all 4 cells)
//      risk_pct       = 0.005          (0.5% of equity per trade)
//      start_equity   = $10,000        (paper, shadow only)
//      margin_call    = $1,000
//
//  HOW THIS INTEGRATES WITH THE EXISTING RUNTIME
//  ---------------------------------------------
//  This engine is self-contained -- a single header, no edits to existing
//  engine code. It plugs into the runtime via three small additions:
//
//      1. globals.hpp        : declare static omega::C1RetunedPortfolio g_c1_retuned;
//      2. engine_init.hpp    : call g_c1_retuned.init(...) inside the same init function
//                              that sets up MinimalH4Breakout (around line 406).
//      3. tick_gold.hpp      : call g_c1_retuned.on_h1_bar(...) where on_h4_bar() and
//                              g_minimal_h4_gold.on_h4_bar() are dispatched (around line 793).
//                              Call g_c1_retuned.on_tick(bid, ask, now_ms, ...) where
//                              g_minimal_h4_gold.on_tick(...) is called.
//
//  H1 bars come from g_bars_gold.h1 (existing infrastructure). H4 bars come
//  from g_bars_gold.h4. H2 and H6 do NOT exist in g_bars_gold so this engine
//  synthesizes them internally from completed H1 bars (2 H1 = 1 H2; 6 H1 =
//  1 H6) and computes its own Bollinger bands + ATR14 on those synthesized
//  bars. This keeps the integration narrow: H1 bar input is the only signal
//  the runtime has to feed in.
//
//  SHADOW-MODE DEFAULT
//  -------------------
//  shadow_mode default true. Trades are emitted via the same on_close
//  callback as MinimalH4Breakout, which routes them through the existing
//  shadow CSV pipeline (logs/shadow/omega_shadow.csv). Each TradeRecord is
//  stamped with engine="C1Retuned_<cell>" and shadow=true so the existing
//  scripts/shadow_analysis.py picks them up unchanged.
//
//  HALT CRITERIA
//  -------------
//  Per CHOSEN.md: cluster days (4 cells losing same UTC session) >2x
//  expected frequency in first 2 weeks -> pause and re-evaluate. Tracked
//  internally by C1RetunedPortfolio (cluster_days_, daily_pnl_), surfaced
//  via halt_status() so a wrapper can call it from on_tick on a 5-minute
//  cadence. The halt logic only emits a flag: it does NOT auto-disable
//  cells. A human decides what to do with the flag.
//
//  RULES FOLLOWED
//  --------------
//  - Self-contained: no edits to existing engine headers.
//  - Long-only: short signals are not generated.
//  - Shadow-default: shadow_mode=true unless explicitly flipped.
//  - No core-code modification: this header does not include any other
//    omega engine header except OmegaTradeLedger.hpp (for TradeRecord) and
//    OHLCBarEngine.hpp (forward-declared use of OHLCBar struct only).
// =============================================================================

#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <string>
#include <vector>
#include "OmegaTradeLedger.hpp"

namespace omega {

// =============================================================================
//  Synthesized OHLC bar -- minimal struct used for H2 / H6 internal aggregation.
//  Independent of OHLCBarEngine to keep this header standalone.
// =============================================================================
struct C1Bar {
    int64_t bar_start_ms = 0;
    double  open  = 0.0;
    double  high  = 0.0;
    double  low   = 0.0;
    double  close = 0.0;
};

// =============================================================================
//  Bar synthesizer: aggregates N H1 bars into one bigger-TF bar.
//      stride = 2 -> H2;  stride = 6 -> H6
//
//  Calling code feeds completed H1 bars in via on_h1_bar(); the synthesizer
//  emits a completed C1Bar via the user-supplied callback whenever stride
//  bars have accumulated.
// =============================================================================
struct C1BarSynth {
    int    stride       = 2;
    int    accum_count_ = 0;
    C1Bar  cur_{};

    using EmitCallback = std::function<void(const C1Bar&)>;

    void on_h1_bar(const C1Bar& h1, EmitCallback emit) noexcept {
        if (accum_count_ == 0) {
            cur_           = h1;
            cur_.bar_start_ms = h1.bar_start_ms;  // start of first H1 bar
            cur_.open      = h1.open;
        } else {
            cur_.high  = std::max(cur_.high, h1.high);
            cur_.low   = std::min(cur_.low,  h1.low);
            cur_.close = h1.close;
        }
        accum_count_++;
        if (accum_count_ >= stride) {
            if (emit) emit(cur_);
            accum_count_ = 0;
            cur_ = C1Bar{};
        }
    }
};

// =============================================================================
//  Rolling Bollinger Bands (period=20, k=2) + true-range ATR14 over completed
//  bars. Self-contained -- no shared state with OHLCBarEngine.
// =============================================================================
struct C1Indicators {
    static constexpr int BB_P  = 20;
    static constexpr int ATR_P = 14;
    static constexpr double BB_K = 2.0;

    std::deque<double> closes_;
    std::deque<double> highs_;
    std::deque<double> lows_;
    bool   has_prev_close_ = false;
    double prev_close_     = 0.0;
    double atr_ewm_        = 0.0;  // simple SMA of TR over ATR_P, then EWM
    int    atr_n_          = 0;

    double bb_upper = 0.0;
    double bb_mid   = 0.0;
    double bb_lower = 0.0;
    double atr14    = 0.0;

    bool ready() const noexcept {
        return ((int)closes_.size() >= BB_P) && (atr_n_ >= ATR_P);
    }

    void on_bar(const C1Bar& b) noexcept {
        // Bollinger window
        closes_.push_back(b.close);
        if ((int)closes_.size() > BB_P) closes_.pop_front();
        if ((int)closes_.size() == BB_P) {
            double sum = 0.0;
            for (double c : closes_) sum += c;
            const double mean = sum / BB_P;
            double sq = 0.0;
            for (double c : closes_) { const double d = c - mean; sq += d*d; }
            const double sd = std::sqrt(sq / BB_P);
            bb_mid   = mean;
            bb_upper = mean + BB_K * sd;
            bb_lower = mean - BB_K * sd;
        }

        // True range -> ATR14 (simple SMA over ATR_P bars; sufficient for entry sizing)
        const double tr = has_prev_close_
            ? std::max({ b.high - b.low,
                         std::fabs(b.high - prev_close_),
                         std::fabs(b.low  - prev_close_) })
            : (b.high - b.low);
        highs_.push_back(b.high);
        lows_ .push_back(b.low);
        if ((int)highs_.size() > ATR_P) {
            highs_.pop_front();
            lows_ .pop_front();
        }
        if (atr_n_ < ATR_P) {
            // accumulate simple SMA bootstrap
            atr_ewm_ = (atr_ewm_ * atr_n_ + tr) / (atr_n_ + 1);
            atr_n_++;
        } else {
            // Wilder's smoothing once warmed up
            atr_ewm_ = (atr_ewm_ * (ATR_P - 1) + tr) / ATR_P;
        }
        atr14 = atr_ewm_;

        prev_close_     = b.close;
        has_prev_close_ = true;
    }
};

// =============================================================================
//  Generic open-position struct shared by all 4 cells.
// =============================================================================
struct C1OpenPos {
    bool    active      = false;
    double  entry       = 0.0;
    double  sl          = 0.0;
    double  tp          = 0.0;       // 0 if no fixed TP (Bollinger uses indicator exit)
    double  size        = 0.0;
    double  atr_at_open = 0.0;
    int64_t entry_ts_ms = 0;
    int     bars_held   = 0;
    double  mfe         = 0.0;
    double  mae         = 0.0;
};

// =============================================================================
//  Cell 1: Donchian H1 long retuned (period=20, sl=3.0 ATR, tp=5.0 ATR).
// =============================================================================
struct C1DonchianH1LongCell {
    using CloseCallback = std::function<void(const TradeRecord&)>;

    bool   shadow_mode = true;
    bool   enabled     = true;
    int    period      = 20;
    double sl_atr      = 3.0;
    double tp_atr      = 5.0;       // = tp_r * sl_atr in sim_family_a vocab
    int    max_hold_bars = 30;
    double max_spread  = 1.0;        // pts; tightened from MinimalH4 default for H1 entries

    std::string symbol = "XAUUSD";
    std::string cell_id = "C1Retuned_donchian_H1_long";

    std::deque<double> highs_;
    std::deque<double> lows_;
    double channel_high_ = 0.0;
    double channel_low_  = 1e9;
    int    h1_bar_count_ = 0;

    C1OpenPos pos_{};
    int       trade_id_ = 0;

    bool has_open_position() const noexcept { return pos_.active; }

    // Called on every completed H1 bar.
    // Returns 1 if a NEW position was opened this bar, 0 otherwise.
    int on_h1_bar(const C1Bar& h1, double bid, double ask, double h1_atr14,
                  int64_t now_ms, double size_lot, CloseCallback on_close) noexcept {
        ++h1_bar_count_;

        // Capture prior channel BEFORE pushing this bar (matches backtest semantics).
        const bool channel_ready = ((int)highs_.size() >= period);
        const double prior_high = channel_high_;

        highs_.push_back(h1.high);
        lows_ .push_back(h1.low);
        if ((int)highs_.size() > period) {
            highs_.pop_front();
            lows_ .pop_front();
        }
        if ((int)highs_.size() >= period) {
            channel_high_ = *std::max_element(highs_.begin(), highs_.end());
            channel_low_  = *std::min_element(lows_ .begin(), lows_ .end());
        }

        if (pos_.active) {
            pos_.bars_held++;
            if (pos_.bars_held >= max_hold_bars) _close(bid, "TIMEOUT", now_ms, on_close);
            return 0;
        }

        if (!enabled || !channel_ready || h1_atr14 <= 0.0) return 0;
        if ((ask - bid) > max_spread) return 0;
        if (prior_high <= 0.0) return 0;

        // Long-only Donchian breakout: bar closes above prior 20h channel high.
        if (h1.close <= prior_high) return 0;

        const double entry_px = ask;                     // long-only -> ask
        const double sl_pts   = h1_atr14 * sl_atr;
        const double tp_pts   = h1_atr14 * tp_atr;
        const double sl_px    = entry_px - sl_pts;
        const double tp_px    = entry_px + tp_pts;

        pos_.active      = true;
        pos_.entry       = entry_px;
        pos_.sl          = sl_px;
        pos_.tp          = tp_px;
        pos_.size        = std::max(0.01, size_lot);
        pos_.atr_at_open = h1_atr14;
        pos_.entry_ts_ms = now_ms;
        pos_.bars_held   = 0;
        pos_.mfe = 0.0;
        pos_.mae = 0.0;
        ++trade_id_;

        printf("[C1RETUNED-DONCHIAN_H1] ENTRY LONG @ %.2f sl=%.2f tp=%.2f size=%.4f"
               " atr=%.3f ch_hi=%.2f close=%.2f%s\n",
               entry_px, sl_px, tp_px, pos_.size, h1_atr14, prior_high, h1.close,
               shadow_mode ? " [SHADOW]" : "");
        fflush(stdout);
        return 1;
    }

    // Called on every tick to manage open position.
    void on_tick(double bid, double ask, int64_t now_ms,
                 CloseCallback on_close) noexcept {
        if (!pos_.active) return;
        const double mfe_now = bid - pos_.entry;
        const double mae_now = pos_.entry - ask;
        if (mfe_now > pos_.mfe) pos_.mfe = mfe_now;
        if (mae_now > pos_.mae) pos_.mae = mae_now;
        if (bid <= pos_.sl)      _close(bid, "SL_HIT", now_ms, on_close);
        else if (bid >= pos_.tp) _close(bid, "TP_HIT", now_ms, on_close);
    }

    void force_close(double bid, double /*ask*/, int64_t now_ms,
                     CloseCallback cb) noexcept {
        if (pos_.active) _close(bid, "FORCE_CLOSE", now_ms, cb);
    }

private:
    void _close(double exit_px, const char* reason, int64_t now_ms,
                CloseCallback on_close) noexcept {
        const double pnl_pts = (exit_px - pos_.entry) * pos_.size;
        printf("[C1RETUNED-DONCHIAN_H1] EXIT LONG @ %.2f %s pnl=$%.2f bars=%d%s\n",
               exit_px, reason, pnl_pts * 100.0, pos_.bars_held,
               shadow_mode ? " [SHADOW]" : "");
        fflush(stdout);

        if (on_close) {
            TradeRecord tr;
            tr.id         = trade_id_;
            tr.symbol     = symbol;
            tr.side       = "LONG";
            tr.engine     = cell_id;
            tr.entryPrice = pos_.entry;
            tr.exitPrice  = exit_px;
            tr.sl         = pos_.sl;
            tr.tp         = pos_.tp;
            tr.size       = pos_.size;
            tr.pnl        = pnl_pts;
            tr.mfe        = pos_.mfe * pos_.size;
            tr.mae        = pos_.mae * pos_.size;
            tr.entryTs    = pos_.entry_ts_ms / 1000;
            tr.exitTs     = now_ms / 1000;
            tr.exitReason = reason;
            tr.regime     = "C1_RETUNED";
            tr.atr_at_entry = pos_.atr_at_open;
            tr.shadow     = shadow_mode;
            on_close(tr);
        }
        pos_ = C1OpenPos{};
    }
};

// =============================================================================
//  Cells 2/3/4: Bollinger long. Generic class parameterized by timeframe.
//  Entry: bar close <= bb_lower (touch the lower band).
//  Exit:  bar close >= bb_mid   (back to the midline) OR hard ATR SL OR timeout.
//  Long-only.
// =============================================================================
struct C1BollingerLongCell {
    using CloseCallback = std::function<void(const TradeRecord&)>;

    bool   shadow_mode = true;
    bool   enabled     = true;
    double hard_sl_atr = 1.5;        // hard SL distance (intrabar)
    int    max_hold_bars = 20;
    double max_spread  = 1.5;

    std::string symbol  = "XAUUSD";
    std::string cell_id = "C1Retuned_bollinger_Hx_long";  // overridden per-instance

    C1OpenPos pos_{};
    int       trade_id_ = 0;
    int       bar_count_ = 0;

    bool has_open_position() const noexcept { return pos_.active; }

    // Returns 1 if a new long was opened this bar, 0 otherwise.
    int on_bar(const C1Bar& b, double bid, double ask,
               double bb_upper, double bb_mid, double bb_lower, double atr14,
               int64_t now_ms, double size_lot, CloseCallback on_close) noexcept {
        ++bar_count_;

        // Manage open position FIRST: indicator exit on bar close >= midline.
        if (pos_.active) {
            pos_.bars_held++;
            if (b.close >= bb_mid) {
                _close(bid, "INDICATOR", now_ms, on_close);
                return 0;
            }
            if (pos_.bars_held >= max_hold_bars) {
                _close(bid, "TIMEOUT", now_ms, on_close);
                return 0;
            }
            return 0;
        }

        if (!enabled) return 0;
        if (atr14 <= 0.0 || bb_lower <= 0.0 || bb_mid <= 0.0) return 0;
        if ((ask - bid) > max_spread) return 0;

        // Long entry on lower-band touch.
        if (b.close > bb_lower) return 0;

        const double entry_px = ask;
        const double sl_pts   = atr14 * hard_sl_atr;
        const double sl_px    = entry_px - sl_pts;

        pos_.active      = true;
        pos_.entry       = entry_px;
        pos_.sl          = sl_px;
        pos_.tp          = 0.0;            // no fixed TP -- midline indicator exit
        pos_.size        = std::max(0.01, size_lot);
        pos_.atr_at_open = atr14;
        pos_.entry_ts_ms = now_ms;
        pos_.bars_held   = 0;
        pos_.mfe = 0.0;
        pos_.mae = 0.0;
        ++trade_id_;

        printf("[%s] ENTRY LONG @ %.2f sl=%.2f size=%.4f atr=%.3f"
               " bb_lo=%.2f bb_mid=%.2f close=%.2f%s\n",
               cell_id.c_str(), entry_px, sl_px, pos_.size, atr14,
               bb_lower, bb_mid, b.close, shadow_mode ? " [SHADOW]" : "");
        fflush(stdout);
        return 1;
    }

    void on_tick(double bid, double ask, int64_t now_ms,
                 CloseCallback on_close) noexcept {
        if (!pos_.active) return;
        const double mfe_now = bid - pos_.entry;
        const double mae_now = pos_.entry - ask;
        if (mfe_now > pos_.mfe) pos_.mfe = mfe_now;
        if (mae_now > pos_.mae) pos_.mae = mae_now;
        if (bid <= pos_.sl) _close(bid, "SL_HIT", now_ms, on_close);
    }

    void force_close(double bid, double /*ask*/, int64_t now_ms,
                     CloseCallback cb) noexcept {
        if (pos_.active) _close(bid, "FORCE_CLOSE", now_ms, cb);
    }

private:
    void _close(double exit_px, const char* reason, int64_t now_ms,
                CloseCallback on_close) noexcept {
        const double pnl_pts = (exit_px - pos_.entry) * pos_.size;
        printf("[%s] EXIT LONG @ %.2f %s pnl=$%.2f bars=%d%s\n",
               cell_id.c_str(), exit_px, reason, pnl_pts * 100.0,
               pos_.bars_held, shadow_mode ? " [SHADOW]" : "");
        fflush(stdout);

        if (on_close) {
            TradeRecord tr;
            tr.id         = trade_id_;
            tr.symbol     = symbol;
            tr.side       = "LONG";
            tr.engine     = cell_id;
            tr.entryPrice = pos_.entry;
            tr.exitPrice  = exit_px;
            tr.sl         = pos_.sl;
            tr.tp         = pos_.tp;
            tr.size       = pos_.size;
            tr.pnl        = pnl_pts;
            tr.mfe        = pos_.mfe * pos_.size;
            tr.mae        = pos_.mae * pos_.size;
            tr.entryTs    = pos_.entry_ts_ms / 1000;
            tr.exitTs     = now_ms / 1000;
            tr.exitReason = reason;
            tr.regime     = "C1_RETUNED";
            tr.atr_at_entry = pos_.atr_at_open;
            tr.shadow     = shadow_mode;
            on_close(tr);
        }
        pos_ = C1OpenPos{};
    }
};

// =============================================================================
//  C1RetunedPortfolio -- orchestrator. Owns the 4 cells, enforces
//  max_concurrent and 0.5% risk sizing, exposes a single bar-dispatch entry
//  point and a single tick-dispatch entry point.
//
//  USAGE
//  -----
//      g_c1_retuned.init();
//
//      // In tick_gold.hpp inside the H1-bar-close block (after g_bars_gold.h1.add_bar(s_cur_h1)):
//      omega::C1Bar h1{};
//      h1.bar_start_ms = s_bar_h1_ms;
//      h1.open  = s_cur_h1.open;
//      h1.high  = s_cur_h1.high;
//      h1.low   = s_cur_h1.low;
//      h1.close = s_cur_h1.close;
//      g_c1_retuned.on_h1_bar(h1, bid, ask,
//          g_bars_gold.h1.ind.atr14.load(std::memory_order_relaxed),
//          now_ms_g, ca_on_close);
//
//      // In tick_gold.hpp inside the H4-bar-close block (after g_bars_gold.h4.add_bar(s_cur_h4)):
//      omega::C1Bar h4{};
//      h4.bar_start_ms = s_bar_h4_ms;
//      h4.open  = s_cur_h4.open;
//      h4.high  = s_cur_h4.high;
//      h4.low   = s_cur_h4.low;
//      h4.close = s_cur_h4.close;
//      g_c1_retuned.on_h4_bar(h4, bid, ask,
//          g_bars_gold.h4.ind.bb_upper.load(std::memory_order_relaxed),
//          g_bars_gold.h4.ind.bb_mid  .load(std::memory_order_relaxed),
//          g_bars_gold.h4.ind.bb_lower.load(std::memory_order_relaxed),
//          g_bars_gold.h4.ind.atr14   .load(std::memory_order_relaxed),
//          now_ms_g, ca_on_close);
//
//      // In tick_gold.hpp on every tick (alongside g_minimal_h4_gold.on_tick):
//      g_c1_retuned.on_tick(bid, ask, now_ms_g, ca_on_close);
//
//  The orchestrator wraps the on_close callback so it can update its own
//  equity / cluster state before forwarding to the runtime callback.
// =============================================================================
struct C1RetunedPortfolio {
    using CloseCallback = std::function<void(const TradeRecord&)>;

    // Config (hot-reloadable from omega_config.ini section [c1_retuned]).
    bool   enabled        = true;
    bool   shadow_mode    = true;          // applies to all 4 cells
    int    max_concurrent = 4;
    double risk_pct       = 0.005;         // 0.5%
    double start_equity   = 10000.0;
    double margin_call    = 1000.0;
    double max_lot_cap    = 0.10;          // hard ceiling per single cell entry

    // Cells.
    C1DonchianH1LongCell donchian_h1_;
    C1BollingerLongCell  bollinger_h2_;
    C1BollingerLongCell  bollinger_h4_;
    C1BollingerLongCell  bollinger_h6_;

    // Internal H2 / H6 bar synthesis.
    C1BarSynth   synth_h2_;     // stride=2
    C1BarSynth   synth_h6_;     // stride=6
    C1Indicators ind_h2_;       // BB+ATR14 over synthesized H2 bars
    C1Indicators ind_h6_;       // BB+ATR14 over synthesized H6 bars

    // Equity / cluster tracking (paper, shadow only).
    double equity_       = 10000.0;
    double peak_equity_  = 10000.0;
    double max_dd_pct_   = 0.0;
    int    open_count_   = 0;
    int    blocked_max_concurrent_ = 0;

    // Per-day cluster tracking (UTC day).
    int64_t cur_day_         = 0;
    int     cells_lost_today_ = 0;       // cells that have closed a loser today
    bool    cell_lost_today_[4] = {false,false,false,false};
    int     cluster_days_total_ = 0;     // running count of cluster days >= 4

    // Halt state.
    bool   halt_tripped_ = false;
    std::string halt_reason_;

    void init() {
        donchian_h1_.symbol  = "XAUUSD";
        donchian_h1_.cell_id = "C1Retuned_donchian_H1_long";
        donchian_h1_.shadow_mode = shadow_mode;

        bollinger_h2_.symbol  = "XAUUSD";
        bollinger_h2_.cell_id = "C1Retuned_bollinger_H2_long";
        bollinger_h2_.shadow_mode = shadow_mode;

        bollinger_h4_.symbol  = "XAUUSD";
        bollinger_h4_.cell_id = "C1Retuned_bollinger_H4_long";
        bollinger_h4_.shadow_mode = shadow_mode;

        bollinger_h6_.symbol  = "XAUUSD";
        bollinger_h6_.cell_id = "C1Retuned_bollinger_H6_long";
        bollinger_h6_.shadow_mode = shadow_mode;

        synth_h2_.stride = 2;
        synth_h6_.stride = 6;

        equity_      = start_equity;
        peak_equity_ = start_equity;

        printf("[C1RETUNED-INIT] portfolio shadow=%s max_conc=%d risk_pct=%.4f"
               " equity=%.2f cells=donchian_H1+bollinger_H2/H4/H6\n",
               shadow_mode ? "true" : "false", max_concurrent, risk_pct,
               equity_);
        fflush(stdout);
    }

    // Compute lot from $-risk and SL distance in price points.
    // size = (equity * risk_pct) / (sl_pts * USD_PER_POINT_PER_LOT * 100)
    // (USD_PER_POINT_PER_LOT = 1.0 for XAUUSD; *100 because lot=1.0 is 100oz)
    double size_for(double sl_pts) const noexcept {
        if (sl_pts <= 0.0) return 0.01;
        const double risk_target = equity_ * risk_pct;
        const double risk_per_lot = sl_pts * 100.0;
        if (risk_per_lot <= 0.0) return 0.01;
        double lot = risk_target / risk_per_lot;
        lot = std::floor(lot / 0.01) * 0.01;
        return std::max(0.01, std::min(max_lot_cap, lot));
    }

    bool can_open() const noexcept {
        if (!enabled) return false;
        if (halt_tripped_) return false;
        if (equity_ < margin_call) return false;
        return open_count_ < max_concurrent;
    }

    int total_open() const noexcept {
        return (donchian_h1_.has_open_position() ? 1 : 0)
             + (bollinger_h2_.has_open_position() ? 1 : 0)
             + (bollinger_h4_.has_open_position() ? 1 : 0)
             + (bollinger_h6_.has_open_position() ? 1 : 0);
    }

    // Wrapper around the runtime's on_close callback. Updates equity +
    // cluster tracking, then forwards.
    CloseCallback wrap(CloseCallback runtime_cb, int cell_idx) {
        return [this, runtime_cb, cell_idx](const TradeRecord& tr) {
            const double pnl_usd = tr.pnl * 100.0;
            equity_ += pnl_usd;
            if (equity_ > peak_equity_) peak_equity_ = equity_;
            const double dd = (equity_ - peak_equity_) / peak_equity_;
            if (dd < max_dd_pct_) max_dd_pct_ = dd;

            const int64_t day = (tr.exitTs) / 86400LL;
            if (day != cur_day_) {
                cur_day_ = day;
                std::fill(std::begin(cell_lost_today_), std::end(cell_lost_today_), false);
                cells_lost_today_ = 0;
            }
            if (pnl_usd < 0.0 && cell_idx >= 0 && cell_idx < 4
                && !cell_lost_today_[cell_idx]) {
                cell_lost_today_[cell_idx] = true;
                cells_lost_today_++;
                if (cells_lost_today_ >= 4) {
                    cluster_days_total_++;
                    halt_reason_ = "cluster_day: 4 cells losing same UTC session";
                    // Light flag only -- portfolio does NOT auto-disable cells.
                    halt_tripped_ = (max_dd_pct_ <= -0.075)
                                  || (cluster_days_total_ >= 1);
                }
            }
            if (max_dd_pct_ <= -0.075) {
                halt_tripped_ = true;
                halt_reason_ = "DD breach: max_dd <= -7.5%";
            }
            open_count_ = std::max(0, open_count_ - 1);

            printf("[C1RETUNED-CLOSE] cell=%s pnl=$%.2f equity=$%.2f"
                   " peak=$%.2f dd=%.2f%% cluster_days=%d open=%d halt=%s\n",
                   tr.engine.c_str(), pnl_usd, equity_, peak_equity_,
                   max_dd_pct_ * 100.0, cluster_days_total_, open_count_,
                   halt_tripped_ ? "YES" : "no");
            fflush(stdout);

            if (runtime_cb) runtime_cb(tr);
        };
    }

    // -------------------------------------------------------------------------
    //  H1 bar dispatch -- drives Donchian H1 cell + synthesised H2/H6 cells.
    //  Call this from the H1-close handler in tick_gold.hpp (after add_bar).
    // -------------------------------------------------------------------------
    void on_h1_bar(const C1Bar& h1, double bid, double ask, double h1_atr14,
                   int64_t now_ms, CloseCallback runtime_cb) noexcept {
        if (!enabled) return;

        // ---------- Cell 1: Donchian H1 long --------------------------------
        if (donchian_h1_.has_open_position()) {
            (void)donchian_h1_.on_h1_bar(h1, bid, ask, h1_atr14, now_ms,
                                         0.0, wrap(runtime_cb, 0));
        } else if (can_open()) {
            const double sl_pts = h1_atr14 * donchian_h1_.sl_atr;
            const double lot    = size_for(sl_pts);
            const int opened = donchian_h1_.on_h1_bar(h1, bid, ask, h1_atr14,
                                                     now_ms, lot, wrap(runtime_cb, 0));
            if (opened) ++open_count_;
        } else {
            // wanted to fire but blocked by max_concurrent
            ++blocked_max_concurrent_;
        }

        // ---------- Cell 2: Bollinger H2 long (synthesized) ------------------
        synth_h2_.on_h1_bar(h1, [&](const C1Bar& h2_bar) {
            ind_h2_.on_bar(h2_bar);
            if (!ind_h2_.ready()) return;
            if (bollinger_h2_.has_open_position()) {
                (void)bollinger_h2_.on_bar(h2_bar, bid, ask,
                    ind_h2_.bb_upper, ind_h2_.bb_mid, ind_h2_.bb_lower,
                    ind_h2_.atr14, now_ms, 0.0, wrap(runtime_cb, 1));
            } else if (can_open()) {
                const double sl_pts = ind_h2_.atr14 * bollinger_h2_.hard_sl_atr;
                const double lot    = size_for(sl_pts);
                const int opened = bollinger_h2_.on_bar(h2_bar, bid, ask,
                    ind_h2_.bb_upper, ind_h2_.bb_mid, ind_h2_.bb_lower,
                    ind_h2_.atr14, now_ms, lot, wrap(runtime_cb, 1));
                if (opened) ++open_count_;
            }
        });

        // ---------- Cell 4: Bollinger H6 long (synthesized) ------------------
        synth_h6_.on_h1_bar(h1, [&](const C1Bar& h6_bar) {
            ind_h6_.on_bar(h6_bar);
            if (!ind_h6_.ready()) return;
            if (bollinger_h6_.has_open_position()) {
                (void)bollinger_h6_.on_bar(h6_bar, bid, ask,
                    ind_h6_.bb_upper, ind_h6_.bb_mid, ind_h6_.bb_lower,
                    ind_h6_.atr14, now_ms, 0.0, wrap(runtime_cb, 3));
            } else if (can_open()) {
                const double sl_pts = ind_h6_.atr14 * bollinger_h6_.hard_sl_atr;
                const double lot    = size_for(sl_pts);
                const int opened = bollinger_h6_.on_bar(h6_bar, bid, ask,
                    ind_h6_.bb_upper, ind_h6_.bb_mid, ind_h6_.bb_lower,
                    ind_h6_.atr14, now_ms, lot, wrap(runtime_cb, 3));
                if (opened) ++open_count_;
            }
        });
    }

    // -------------------------------------------------------------------------
    //  H4 bar dispatch -- drives the Bollinger H4 cell only.
    //  Call this from the H4-close handler in tick_gold.hpp (after add_bar).
    //  Bollinger bands + ATR14 come from g_bars_gold.h4.ind.* (already
    //  computed by OHLCBarEngine on bar close).
    // -------------------------------------------------------------------------
    void on_h4_bar(const C1Bar& h4, double bid, double ask,
                   double h4_bb_upper, double h4_bb_mid, double h4_bb_lower,
                   double h4_atr14,
                   int64_t now_ms, CloseCallback runtime_cb) noexcept {
        if (!enabled) return;
        if (h4_atr14 <= 0.0 || h4_bb_lower <= 0.0) return;
        if (bollinger_h4_.has_open_position()) {
            (void)bollinger_h4_.on_bar(h4, bid, ask,
                h4_bb_upper, h4_bb_mid, h4_bb_lower, h4_atr14,
                now_ms, 0.0, wrap(runtime_cb, 2));
        } else if (can_open()) {
            const double sl_pts = h4_atr14 * bollinger_h4_.hard_sl_atr;
            const double lot    = size_for(sl_pts);
            const int opened = bollinger_h4_.on_bar(h4, bid, ask,
                h4_bb_upper, h4_bb_mid, h4_bb_lower, h4_atr14,
                now_ms, lot, wrap(runtime_cb, 2));
            if (opened) ++open_count_;
        }
    }

    // Tick-level position management for all 4 cells.
    void on_tick(double bid, double ask, int64_t now_ms,
                 CloseCallback runtime_cb) noexcept {
        if (!enabled) return;
        donchian_h1_ .on_tick(bid, ask, now_ms, wrap(runtime_cb, 0));
        bollinger_h2_.on_tick(bid, ask, now_ms, wrap(runtime_cb, 1));
        bollinger_h4_.on_tick(bid, ask, now_ms, wrap(runtime_cb, 2));
        bollinger_h6_.on_tick(bid, ask, now_ms, wrap(runtime_cb, 3));
    }

    // Force-close everything on shutdown.
    void force_close_all(double bid, double ask, int64_t now_ms,
                         CloseCallback runtime_cb) noexcept {
        donchian_h1_ .force_close(bid, ask, now_ms, wrap(runtime_cb, 0));
        bollinger_h2_.force_close(bid, ask, now_ms, wrap(runtime_cb, 1));
        bollinger_h4_.force_close(bid, ask, now_ms, wrap(runtime_cb, 2));
        bollinger_h6_.force_close(bid, ask, now_ms, wrap(runtime_cb, 3));
    }

    // Halt-status accessor for external monitors / dashboards.
    struct HaltStatus {
        bool   ok;
        double equity;
        double peak;
        double max_dd_pct;
        int    cluster_days;
        int    open_count;
        int    blocked_max_concurrent;
        std::string reason;
    };
    HaltStatus halt_status() const noexcept {
        return HaltStatus{
            !halt_tripped_,
            equity_, peak_equity_, max_dd_pct_,
            cluster_days_total_, open_count_,
            blocked_max_concurrent_,
            halt_reason_,
        };
    }
};

} // namespace omega
