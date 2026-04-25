// =============================================================================
//  MinimalH4US30Breakout.hpp  --  Pure H4 Donchian breakout engine for DJ30.F
//
//  Created 2026-04-25. Derived from MinimalH4Breakout.hpp (gold sister engine).
//
//  WHY SELF-CONTAINED:
//      Unlike the gold sister engine which reads g_bars_gold.h4 (full multi-TF
//      bar bundle with H4 OHLC + ATR14), there is no g_bars_us30 in the codebase.
//      Index symbols cannot subscribe to the cTrader trendbar API (BlackBull
//      sends INVALID_REQUEST + TCP RST on pt=2137 for ALL index symbols), so
//      DJ30.F has no external H4 bar feed. This engine therefore builds its own
//      H4 OHLC bars and Wilder ATR14 internally from the live tick stream.
//
//  BACKTEST VALIDATION (Tickstory 2yr DJ30.F, 21.5M ticks, 3,203 H4 bars):
//      27/27 configs profitable on the H4 27-config sweep.
//      Default config (D=10 SL=1.0x TP=4.0x) selected: PnL=+$637, PF=1.54,
//      n=184 trades, WR=28.3%, DD=-$330. Best PF config (D=15 SL=1.0x TP=4.0x)
//      had PF=1.61 but 27 fewer trades; D=10 chosen for higher trade frequency
//      (~0.25/day vs 0.21/day) which gives faster shadow-mode validation.
//      See backtest/htf_bt_US30_results.txt for full sweep.
//
//  Strategy (identical to gold sister):
//    1. H4 bar close above Donchian-N channel high  -> LONG
//    2. H4 bar close below Donchian-N channel low   -> SHORT
//    3. SL = sl_mult * H4_ATR behind entry  (fixed-distance, NOT structural)
//    4. TP = tp_mult * H4_ATR ahead of entry
//    5. No ADX gate, no EMA-sep, no RSI, no M15 expansion, no struct-SL
//    6. Weekend entry gate only (Fri 20:00 UTC through Sun 22:00 UTC)
//    7. Fixed $10 risk per trade, 0.10 lot cap (DJ30.F is $5/pt)
//
//  Exit:
//    - TP hit  (ask <= TP short, bid >= TP long)
//    - SL hit  (ask >= SL short, bid <= SL long)
//    - Timeout after timeout_h4_bars H4 bars (safety valve)
//    - Weekend force-close on Friday 20:00 UTC+ if profitable
//
//  NOTES:
//    - shadow_mode=true default. NEVER set false without N>=10 shadow trade
//      validation matching backtest expectation (~0.25 trades/day = 5/month).
//    - $5/pt for DJ30.F means PnL math: pnl_dollars = pnl_pts * size * 500.0
//      (0.10 lot at $5/pt, scaled). Sizing: $10 / (sl_pts * 500) per lot.
//    - max_spread=3.0pt (DJ30.F has wider spreads than XAUUSD; gold uses 2.0).
//    - H4 bar boundary: floor(now_ms / 14400000) — same as backtest harness.
//    - On cold start the engine needs donchian_bars closed H4 bars before the
//      first signal (10 bars * 4hr = 40hrs). No seeding from disk because no
//      g_bars_us30 exists. This is acceptable: in shadow mode the cost of
//      40hr cold start is zero $$, and warm restart from saved state will be
//      added in a future commit if the engine proves edge in shadow.
// =============================================================================

#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <algorithm>
#include <functional>
#include <string>
#include "OmegaTradeLedger.hpp"

namespace omega {

// =============================================================================
//  Parameter struct -- US30-specific defaults from 2yr Tickstory walk-forward
// =============================================================================
struct MinimalH4US30Params {
    double risk_dollars        = 10.0;   // fixed $ risk per trade (matches gold)
    double max_lot             = 0.10;   // DJ30.F lot cap; $5/pt * 0.10 = $0.50/pt
    double dollars_per_point   = 5.0;    // DJ30.F contract spec ($5/pt at 1.00 lot)
    int    donchian_bars       = 10;     // N for channel lookback (best PnL config)
    double sl_mult             = 1.0;    // SL = 1.0 * H4 ATR behind entry (best PnL)
    double tp_mult             = 4.0;    // TP = 4.0 * H4 ATR ahead of entry (best PnL)
    double max_spread          = 3.0;    // DJ30.F spread is wider than XAUUSD
    int    timeout_h4_bars     = 24;     // 24 H4 bars = 4 days (safety cap)
    int    cooldown_h4_bars    = 2;      // bars between trades
    int    atr_period          = 14;     // Wilder ATR period (matches backtest)
    bool   weekend_close_gate  = true;   // close profitable positions Fri 20:00+ UTC
};

inline MinimalH4US30Params make_minimal_h4_us30_params() {
    MinimalH4US30Params p;
    // Defaults above are OOS-validated on 2yr Tickstory DJ30.F. Keep identical.
    return p;
}

// =============================================================================
//  Signal struct
// =============================================================================
struct MinimalH4US30Signal {
    bool        valid   = false;
    bool        is_long = false;
    double      entry   = 0.0;
    double      sl      = 0.0;
    double      tp      = 0.0;
    double      size    = 0.0;
    const char* reason  = "";
};

// =============================================================================
//  MinimalH4US30Breakout engine
// =============================================================================
struct MinimalH4US30Breakout {

    bool                shadow_mode = true;
    bool                enabled     = true;
    MinimalH4US30Params p;
    std::string         symbol      = "DJ30.F";

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;

    struct OpenPos {
        bool    active       = false;
        bool    is_long      = false;
        double  entry        = 0.0;
        double  sl           = 0.0;
        double  tp           = 0.0;
        double  size         = 0.0;
        double  h4_atr       = 0.0;
        int64_t entry_ts_ms  = 0;
        int     h4_bars_held = 0;
    } pos_;

    // ── Internal H4 OHLC accumulator (ticks → H4 bars) ──────────────────────
    // Tracks the currently-forming H4 bar. When the H4 boundary crosses
    // (now_ms / 14400000 changes), the bar is "closed" -- pushed to history,
    // ATR is updated, Donchian channel is recomputed, and Donchian breakout
    // logic runs against this bar's close price.
    struct H4AccumState {
        bool     active     = false;
        int64_t  bucket_ms  = 0;     // floor(now_ms / 14400000) * 14400000
        double   open       = 0.0;
        double   high       = 0.0;
        double   low        = 0.0;
        double   close      = 0.0;
    } h4_acc_;

    // ── Closed H4 bar history (for Donchian + ATR) ──────────────────────────
    std::deque<double> h4_highs_;       // for Donchian channel high
    std::deque<double> h4_lows_;        // for Donchian channel low
    std::deque<double> h4_closes_;      // for ATR true-range computation
    double  channel_high_       = 0.0;
    double  channel_low_        = 1e9;

    // ── Wilder ATR14 internal state ─────────────────────────────────────────
    // Backtest used SMA-of-TR for first atr_period bars, then Wilder smoothing
    // EWMA with alpha = 1/atr_period thereafter. Mirrored exactly here.
    double  atr_                = 0.0;   // current ATR14 (0 until atr_period bars seen)
    int     atr_seed_count_     = 0;     // counts bars during SMA seed phase
    double  atr_seed_sum_       = 0.0;   // running sum of TR during seed phase
    double  prev_h4_close_      = 0.0;   // for true-range calc on next bar

    // ── Session/state ───────────────────────────────────────────────────────
    double  daily_pnl_          = 0.0;
    int64_t daily_reset_day_    = 0;
    int     h4_bar_count_       = 0;     // total closed H4 bars seen (for cooldown)
    int     cooldown_until_bar_ = 0;
    int     m_trade_id_         = 0;

    bool has_open_position() const noexcept { return pos_.active; }

    // ── Tick handler -- builds H4 OHLC bars internally ──────────────────────
    // Called on every DJ30.F tick. Handles:
    //   1. H4 OHLC accumulation (open/high/low/close as ticks arrive)
    //   2. H4 bar close detection (boundary crossed -> close current bar,
    //      update history, update ATR, run Donchian breakout logic)
    //   3. Open-position tick management (SL/TP check)
    //
    // Returns a signal struct (valid=true only on the tick that triggered an
    // entry on H4 bar close).
    MinimalH4US30Signal on_tick(double bid, double ask, int64_t now_ms,
                                CloseCallback on_close) noexcept
    {
        MinimalH4US30Signal sig{};

        if (bid <= 0.0 || ask <= 0.0) return sig;
        const double mid = (bid + ask) * 0.5;

        _daily_reset(now_ms);

        // ── 1. Open-position tick management (SL/TP) ────────────────────────
        if (pos_.active) {
            if (pos_.is_long) {
                if (bid <= pos_.sl)      _close(bid, "SL_HIT", now_ms, on_close);
                else if (bid >= pos_.tp) _close(bid, "TP_HIT", now_ms, on_close);
            } else {
                if (ask >= pos_.sl)      _close(ask, "SL_HIT", now_ms, on_close);
                else if (ask <= pos_.tp) _close(ask, "TP_HIT", now_ms, on_close);
            }
        }

        // ── 2. H4 OHLC accumulator ──────────────────────────────────────────
        const int64_t bucket = (now_ms / 14400000LL) * 14400000LL;

        if (!h4_acc_.active) {
            // First tick ever: start the accumulator. We do NOT close this bar
            // until the next H4 boundary crosses (no synthetic close).
            h4_acc_.active    = true;
            h4_acc_.bucket_ms = bucket;
            h4_acc_.open      = mid;
            h4_acc_.high      = mid;
            h4_acc_.low       = mid;
            h4_acc_.close     = mid;
            return sig;
        }

        if (bucket != h4_acc_.bucket_ms) {
            // ── H4 BOUNDARY CROSSED -- close the current bar ──
            const double bar_high  = h4_acc_.high;
            const double bar_low   = h4_acc_.low;
            const double bar_close = h4_acc_.close;

            // Capture PRIOR-window Donchian state BEFORE updating with this bar.
            // This matches backtest: signal is "this bar's close vs the channel
            // built from the PRIOR donchian_bars closed bars".
            const bool channel_ready_pre = ((int)h4_highs_.size() >= p.donchian_bars);
            const double prior_high      = channel_high_;
            const double prior_low       = channel_low_;
            const double atr_pre         = atr_;  // ATR using prior bars

            // Update history deques with the closed bar
            h4_highs_ .push_back(bar_high);
            h4_lows_  .push_back(bar_low);
            h4_closes_.push_back(bar_close);
            if ((int)h4_highs_.size() > p.donchian_bars) {
                h4_highs_.pop_front();
                h4_lows_ .pop_front();
            }
            // ATR window can be larger than Donchian window -- keep atr_period+1
            // closes (one extra for prev_close on next TR computation)
            const int atr_keep = p.atr_period + 1;
            while ((int)h4_closes_.size() > atr_keep) h4_closes_.pop_front();

            if ((int)h4_highs_.size() >= p.donchian_bars) {
                channel_high_ = *std::max_element(h4_highs_.begin(), h4_highs_.end());
                channel_low_  = *std::min_element(h4_lows_ .begin(), h4_lows_ .end());
            }

            // Update ATR14 with this bar's true range
            _update_atr_on_bar_close(bar_high, bar_low, bar_close);

            ++h4_bar_count_;

            // ── Run Donchian breakout entry check on this just-closed bar ──
            // Use PRIOR channel + PRIOR ATR (the values before this bar was
            // appended). This matches the backtest harness exactly.
            if (!pos_.active && enabled
                && h4_bar_count_ >= cooldown_until_bar_
                && channel_ready_pre
                && prior_high > 0.0 && prior_low < 1e8
                && atr_pre > 0.0)
            {
                if (_is_weekend_gated(now_ms)) {
                    static int64_t s_wk = 0;
                    if (now_ms - s_wk > 3600000LL) {
                        s_wk = now_ms;
                        printf("[MINIMAL_H4-%s] Weekend gate: no new entries\n",
                               symbol.c_str());
                        fflush(stdout);
                    }
                } else if ((ask - bid) > p.max_spread) {
                    // spread too wide -- skip entry
                } else {
                    const bool bull_break = (bar_close > prior_high);
                    const bool bear_break = (bar_close < prior_low);

                    if (bull_break || bear_break) {
                        const bool intend_long = bull_break;
                        const double entry_px  = intend_long ? ask : bid;
                        const double sl_pts    = atr_pre * p.sl_mult;
                        const double tp_pts    = atr_pre * p.tp_mult;
                        const double sl_px     = intend_long
                            ? (entry_px - sl_pts) : (entry_px + sl_pts);
                        const double tp_px     = intend_long
                            ? (entry_px + tp_pts) : (entry_px - tp_pts);

                        // Sizing: $risk_dollars / (sl_pts * dollars_per_point per lot)
                        // DJ30.F: $5/pt at 1.0 lot, so $0.05/pt at 0.01 lot.
                        // size = risk / (sl_pts * dollars_per_point) where size is in lots.
                        double size = p.risk_dollars
                                    / (sl_pts * p.dollars_per_point);
                        size = std::floor(size / 0.01) * 0.01;
                        size = std::max(0.01, std::min(p.max_lot, size));

                        pos_.active        = true;
                        pos_.is_long       = intend_long;
                        pos_.entry         = entry_px;
                        pos_.sl            = sl_px;
                        pos_.tp            = tp_px;
                        pos_.size          = size;
                        pos_.h4_atr        = atr_pre;
                        pos_.entry_ts_ms   = now_ms;
                        pos_.h4_bars_held  = 0;
                        ++m_trade_id_;

                        const double ch_level = intend_long ? prior_high : prior_low;
                        printf("[MINIMAL_H4-%s] ENTRY %s @ %.2f sl=%.2f tp=%.2f"
                               " size=%.3f h4_atr=%.2f channel_%s=%.2f close=%.2f%s\n",
                               symbol.c_str(), intend_long ? "LONG" : "SHORT",
                               entry_px, sl_px, tp_px, size, atr_pre,
                               intend_long ? "high" : "low", ch_level, bar_close,
                               shadow_mode ? " [SHADOW]" : "");
                        fflush(stdout);

                        sig.valid   = true;
                        sig.is_long = intend_long;
                        sig.entry   = entry_px;
                        sig.sl      = sl_px;
                        sig.tp      = tp_px;
                        sig.size    = size;
                        sig.reason  = "MINIMAL_H4_DONCHIAN_BREAK";
                    }
                }
            }

            // Increment hold-counter for any open position carrying across H4 bar
            if (pos_.active) {
                pos_.h4_bars_held++;
                _manage_timeout(bid, ask, now_ms, on_close);
            }

            // Start the new H4 accumulator with this tick
            h4_acc_.bucket_ms = bucket;
            h4_acc_.open      = mid;
            h4_acc_.high      = mid;
            h4_acc_.low       = mid;
            h4_acc_.close     = mid;
        } else {
            // Same H4 bucket -- update high/low/close
            if (mid > h4_acc_.high) h4_acc_.high = mid;
            if (mid < h4_acc_.low)  h4_acc_.low  = mid;
            h4_acc_.close = mid;
        }

        return sig;
    }

    // ── Weekend close: close profitable positions Friday 20:00+ UTC ─────────
    void check_weekend_close(double bid, double ask, int64_t now_ms,
                             CloseCallback on_close) noexcept
    {
        if (!pos_.active || !p.weekend_close_gate) return;
        const int64_t utc_sec  = now_ms / 1000LL;
        const int     utc_dow  = static_cast<int>((utc_sec / 86400LL + 3) % 7);
        const int     utc_hour = static_cast<int>((utc_sec % 86400LL) / 3600LL);
        const bool is_friday   = (utc_dow == 4);
        if (!is_friday || utc_hour < 20) return;
        const double mid  = (bid + ask) * 0.5;
        const double move = pos_.is_long ? (mid - pos_.entry) : (pos_.entry - mid);
        if (move > 0.0) {
            static int64_t s_wk_close = 0;
            if (now_ms - s_wk_close > 3600000LL) {
                s_wk_close = now_ms;
                printf("[MINIMAL_H4-%s] Weekend close: profitable position closed"
                       " Friday 20:00+\n", symbol.c_str());
                fflush(stdout);
                _close(pos_.is_long ? bid : ask, "WEEKEND_CLOSE", now_ms, on_close);
            }
        }
    }

    void patch_size(double lot) noexcept { if (pos_.active) pos_.size = lot; }
    void cancel()                noexcept { pos_ = OpenPos{}; }

    void force_close(double bid, double ask, int64_t now_ms, CloseCallback cb) noexcept
    {
        if (!pos_.active) return;
        _close(pos_.is_long ? bid : ask, "FORCE_CLOSE", now_ms, cb);
    }

private:
    // Weekend gate: Fri 20:00 UTC+ through Sun 22:00 UTC blocked
    // Mapping: Mon=0 Tue=1 Wed=2 Thu=3 Fri=4 Sat=5 Sun=6.
    // Epoch day 0 = Thu 1970-01-01, so dow = (day + 3) % 7.
    static bool _is_weekend_gated(int64_t now_ms) noexcept {
        const int64_t utc_sec  = now_ms / 1000LL;
        const int     utc_dow  = static_cast<int>((utc_sec / 86400LL + 3) % 7);
        const int     utc_hour = static_cast<int>((utc_sec % 86400LL) / 3600LL);
        if (utc_dow == 4 && utc_hour >= 20) return true;  // Fri 20:00+
        if (utc_dow == 5) return true;                     // Sat all day
        if (utc_dow == 6 && utc_hour < 22)  return true;   // Sun before 22:00
        return false;
    }

    void _daily_reset(int64_t now_ms) noexcept {
        const int64_t day = (now_ms / 1000LL) / 86400LL;
        if (day != daily_reset_day_) { daily_pnl_ = 0.0; daily_reset_day_ = day; }
    }

    void _manage_timeout(double bid, double ask, int64_t now_ms,
                          CloseCallback on_close) noexcept
    {
        if (pos_.h4_bars_held >= p.timeout_h4_bars) {
            printf("[MINIMAL_H4-%s] TIMEOUT %d H4 bars\n",
                   symbol.c_str(), pos_.h4_bars_held);
            fflush(stdout);
            _close(pos_.is_long ? bid : ask, "TIMEOUT", now_ms, on_close);
        }
    }

    // ── Wilder ATR14 update on H4 bar close ─────────────────────────────────
    // First atr_period bars: collect TR sum -> SMA. Subsequent bars: Wilder
    // smoothing: ATR_new = (ATR_prev * (n-1) + TR_curr) / n.
    void _update_atr_on_bar_close(double bar_high, double bar_low,
                                  double bar_close) noexcept
    {
        // Need a prior close to compute true range. On the very first closed
        // bar, prev_h4_close_ is 0 -- use (bar_high - bar_low) as TR proxy.
        double tr;
        if (prev_h4_close_ <= 0.0) {
            tr = bar_high - bar_low;
        } else {
            const double a = bar_high - bar_low;
            const double b = std::abs(bar_high - prev_h4_close_);
            const double c = std::abs(bar_low  - prev_h4_close_);
            tr = std::max({a, b, c});
        }
        prev_h4_close_ = bar_close;

        if (atr_seed_count_ < p.atr_period) {
            atr_seed_sum_ += tr;
            ++atr_seed_count_;
            if (atr_seed_count_ == p.atr_period) {
                atr_ = atr_seed_sum_ / static_cast<double>(p.atr_period);
                printf("[MINIMAL_H4-%s] ATR14 seeded: %.2f after %d bars\n",
                       symbol.c_str(), atr_, p.atr_period);
                fflush(stdout);
            }
        } else {
            // Wilder smoothing
            const double n = static_cast<double>(p.atr_period);
            atr_ = (atr_ * (n - 1.0) + tr) / n;
        }
    }

    void _close(double exit_px, const char* reason,
                int64_t now_ms, CloseCallback on_close) noexcept
    {
        // PnL calc: pnl_dollars = pnl_pts * size * dollars_per_point.
        // pos_.size is in lots, dollars_per_point is $/pt at 1.0 lot.
        // Example: 30pt move on 0.01 lot DJ30.F = 30 * 0.01 * 5.0 = $1.50.
        const double pnl_pts = pos_.is_long
            ? (exit_px - pos_.entry) : (pos_.entry - exit_px);
        const double pnl_dollars = pnl_pts * pos_.size * p.dollars_per_point;
        daily_pnl_ += pnl_dollars;

        printf("[MINIMAL_H4-%s] EXIT %s @ %.2f %s pnl=$%.2f bars=%d%s\n",
               symbol.c_str(), pos_.is_long ? "LONG" : "SHORT",
               exit_px, reason, pnl_dollars,
               pos_.h4_bars_held, shadow_mode ? " [SHADOW]" : "");
        fflush(stdout);

        if (on_close) {
            omega::TradeRecord tr;
            tr.id         = m_trade_id_;
            tr.symbol     = symbol.c_str();
            tr.side       = pos_.is_long ? "LONG" : "SHORT";
            tr.engine     = "MinimalH4US30Breakout";
            tr.entryPrice = pos_.entry;
            tr.exitPrice  = exit_px;
            tr.sl         = pos_.sl;
            tr.size       = pos_.size;
            tr.pnl        = pnl_dollars;   // dollars, not points
            tr.mfe        = 0.0;
            tr.mae        = 0.0;
            tr.entryTs    = pos_.entry_ts_ms / 1000;
            tr.exitTs     = now_ms / 1000;
            tr.exitReason = reason;
            tr.regime     = "MINIMAL_H4_US30";
            tr.l2_live    = false;
            tr.shadow     = shadow_mode;
            on_close(tr);
        }
        cooldown_until_bar_ = h4_bar_count_ + p.cooldown_h4_bars;
        pos_ = OpenPos{};
    }
};

} // namespace omega
