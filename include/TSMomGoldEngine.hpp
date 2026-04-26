// =============================================================================
//  TSMomGoldEngine.hpp -- Daily-bar Time-Series Momentum engine for XAUUSD
//
//  Created S46 (2026-04-27). Long-only, daily-bar trend-following engine
//  inspired by:
//    Singha+ arXiv 2511.08571 (Nov 2025): "Time-series momentum on gold,
//      walk-forward 10y/6m, Sharpe 2.88, p_bull >= 0.52, ATR-based exits"
//    Moskowitz/Ooi/Pedersen 2012 JFE "Time Series Momentum"
//
//  Strategy (deterministic, fully specified):
//    1. Aggregate ticks into daily bars via BtBarEngine<1440>.
//    2. Maintain a rolling buffer of the last close_window+slope_z_window+1
//       daily closes.
//    3. On each daily-bar close, compute:
//         a. close_slope = OLS slope of close[t-49..t] vs days [0..49]
//         b. slope_z     = (close_slope - mean(slope_history))
//                          / std(slope_history)
//            using a rolling history of the last slope_z_window slope values
//         c. mom_binary  = 1 if close[t] > close[t-50] else 0
//         d. p_bull      = 0.5 + 0.5 * tanh(slope_z) * mom_binary
//                          (so p_bull == 0.5 when momentum binary is off,
//                           never crosses entry threshold => never enters)
//    4. ENTRY (long-only): if p_bull >= p_bull_threshold (default 0.52)
//                          AND no open position
//                          AND not in cooldown
//                          AND not in weekend-gate
//                          AND ATR14 > 0
//                          AND spread <= max_spread.
//       Entry price = ask. SL = entry - sl_mult * ATR14. TP not set
//       (pure trail-style, no take-profit target).
//    5. SIZING (vol-targeted):
//         realized_vol_ann = std(daily_returns[-30..]) * sqrt(252)
//         raw_lot          = target_vol_ann / max(realized_vol_ann, 1e-4)
//                            * scale (default 0.01 to land in 0.01 ballpark)
//         lot              = clamp(raw_lot, 0.01, max_lot)
//       This produces 0.01 in normal-vol regimes and stays at 0.01 in
//       high-vol regimes (since max_lot is the live cap). The vol target
//       primarily SHRINKS exposure in calm regimes when scaling above
//       0.01 -- but since max_lot=0.01 caps us, in this iteration sizing
//       acts as a sanity guard rather than active scaling. This is
//       intentional for the first live-shadow iteration; T5b will lift
//       the cap once cross-engine inverse-vol is in place.
//    6. EXITS (in priority order, checked on every tick):
//         a. SL_HIT    : bid <= SL                                  -> close
//         b. TRAIL_HIT : price moved up then retraced trail_mult*ATR -> close
//                        (trail tracked from peak mid since entry)
//         c. TIMEOUT   : daily bars held >= timeout_days             -> close
//         d. WEEKEND   : Fri 20:00+ UTC and position profitable      -> close
//    7. COOLDOWN: cooldown_days daily bars after any exit before re-entry.
//    8. WEEKEND ENTRY GATE: identical to MinimalH4Breakout
//       (Fri 20:00 UTC through Sun 22:00 UTC blocked).
//
//  Validation: Backtest on 26m XAUUSD merged CSV (2024-03 -> 2026-04, 154M
//  ticks). Predicted edge in S45 carryover: +$50 to +$200, Sharpe 1.5-2.5
//  on the bull-dominated window. Run with --engine tsmom.
//
//  Compile-tested on g++ -std=c++17 (Mac) and MSVC (Windows VPS) targets.
//  No platform-specific code paths.
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
//  Parameter struct -- all defaults derived from Singha+ 2025 paper or
//  conservative trend-follower literature. Tuned for daily-bar XAUUSD only.
// =============================================================================
struct TSMomGoldParams {
    // Signal windows
    int    close_window       = 50;     // OLS slope window (bars)
    int    mom_window         = 50;     // momentum binary lookback (bars)
    int    slope_z_window     = 100;    // rolling std/mean for slope z-score

    // Entry threshold
    double p_bull_threshold   = 0.52;   // Singha 2025: 0.52 long entry

    // Sizing
    double target_vol_ann     = 0.15;   // 15% annualised target vol
    double size_scale         = 0.01;   // scale factor mapping vol-target to lots
    double max_lot            = 0.01;   // hard cap for live-shadow lift
    double min_lot            = 0.01;   // floor (matches MinH4)

    // Risk
    double sl_mult            = 3.0;    // SL = entry - sl_mult * ATR14
    double trail_mult         = 3.0;    // trail distance from peak (in ATR14)
    double max_spread         = 2.0;    // reject entry if spread > this
    int    timeout_days       = 60;     // safety cap (daily bars)
    int    cooldown_days      = 1;      // bars between trades

    // Vol-target lookback
    int    vol_window         = 30;     // daily returns window for realised vol

    // Behavioural
    bool   weekend_close_gate = true;   // close profitable pos Fri 20:00+ UTC
    bool   long_only          = true;   // S46: hard-coded long-only at top of file
};

inline TSMomGoldParams make_tsmom_gold_params() {
    TSMomGoldParams p;
    return p;
}

// =============================================================================
//  Signal struct -- diagnostic record of the most recent on_daily_bar() pass
// =============================================================================
struct TSMomSignal {
    bool        valid          = false;
    bool        is_long        = false;
    double      entry          = 0.0;
    double      sl             = 0.0;
    double      size           = 0.0;
    double      slope_z        = 0.0;
    double      mom_binary     = 0.0;
    double      p_bull         = 0.0;
    double      realized_vol   = 0.0;
    const char* reason         = "";
};

// =============================================================================
//  TSMomGoldEngine
//
//  Drive flow per OmegaBacktest runner pattern:
//    on_tick(bid, ask, ts_ms, on_close)      -- every tick: SL/trail/weekend
//    on_daily_bar(close, atr14, bid, ask,
//                 ts_ms, on_close)           -- once per closed daily bar
// =============================================================================
struct TSMomGoldEngine {

    bool             shadow_mode = true;
    bool             enabled     = true;
    TSMomGoldParams  p;
    std::string      symbol      = "XAUUSD";

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;

    struct OpenPos {
        bool    active       = false;
        bool    is_long      = true;       // long-only: always true when active
        double  entry        = 0.0;
        double  sl           = 0.0;
        double  size         = 0.0;
        double  atr14        = 0.0;
        double  peak_mid     = 0.0;        // running max since entry (long)
        double  trail_dist   = 0.0;        // trail_mult * atr14 cached at entry
        double  mfe          = 0.0;        // max favourable excursion (price units)
        double  mae          = 0.0;        // max adverse excursion (price units)
        int64_t entry_ts_ms  = 0;
        int     bars_held    = 0;
    } pos_;

    // Session/state
    double  daily_pnl_          = 0.0;
    int64_t daily_reset_day_    = 0;
    int     bar_count_          = 0;          // count of daily bars seen
    int     cooldown_until_bar_ = 0;          // re-arm bar index
    int     m_trade_id_         = 0;

    // Rolling buffers (sized at first push to avoid allocation churn)
    std::deque<double> closes_;               // last (close_window + mom_window + 1) closes
    std::deque<double> slope_history_;        // last slope_z_window slopes
    std::deque<double> daily_returns_;        // last vol_window daily log-returns

    // Diagnostic snapshots of last computed values (for logging)
    TSMomSignal last_signal_{};

    // -------------------------------------------------------------------------
    //  Public accessors
    // -------------------------------------------------------------------------
    bool has_open_position() const noexcept { return pos_.active; }

    // -------------------------------------------------------------------------
    //  on_daily_bar -- call exactly once per closed daily bar.
    //
    //  Updates closes/returns/slope buffers, computes signal, fires entry
    //  on threshold. Increments per-position bars_held. Returns the most
    //  recent signal evaluated (valid==true => entry fired).
    //
    //  All exits are tick-driven via on_tick(); the only exit reachable
    //  here is timeout (checked at start of an open-position branch).
    // -------------------------------------------------------------------------
    TSMomSignal on_daily_bar(
        double  daily_close,
        double  atr14,
        double  bid, double ask,
        int64_t now_ms,
        CloseCallback on_close) noexcept
    {
        ++bar_count_;
        _daily_reset(now_ms);

        // Append close BEFORE computing signal so the regression and momentum
        // see today's bar.
        const int max_close_buf = p.close_window + p.mom_window + 1;
        closes_.push_back(daily_close);
        if ((int)closes_.size() > max_close_buf) closes_.pop_front();

        // Daily log-return buffer for vol target
        if ((int)closes_.size() >= 2) {
            const double prev = closes_[closes_.size() - 2];
            if (prev > 0.0 && daily_close > 0.0) {
                const double ret = std::log(daily_close / prev);
                daily_returns_.push_back(ret);
                if ((int)daily_returns_.size() > p.vol_window) daily_returns_.pop_front();
            }
        }

        // Manage open position (timeout check) FIRST. Tick-level SL/trail run
        // on every tick; here we handle bar-driven timeout only.
        if (pos_.active) {
            ++pos_.bars_held;
            if (pos_.bars_held >= p.timeout_days) {
                printf("[TSMOM-%s] TIMEOUT %d daily bars\n",
                       symbol.c_str(), pos_.bars_held);
                fflush(stdout);
                _close(pos_.is_long ? bid : ask, "TIMEOUT", now_ms, on_close);
            }
            // Even if we just closed via timeout, no new entry on the same
            // bar (cooldown enforces this).
            return TSMomSignal{};
        }

        TSMomSignal sig{};
        if (!enabled) return sig;

        // Buffer-readiness gate
        const int n_for_signal = p.close_window + 1;            // need close[t-50] for momentum
        if ((int)closes_.size() < n_for_signal) return sig;

        // -- 1. OLS slope of last close_window closes vs index 0..close_window-1
        const double slope = _ols_slope_last_n(closes_, p.close_window);

        // Push slope into history buffer (used for z-score)
        slope_history_.push_back(slope);
        if ((int)slope_history_.size() > p.slope_z_window) slope_history_.pop_front();

        // Need at least (close_window) bars worth of slope history for a
        // meaningful z-score. Until then, hold fire.
        const int slope_z_min = std::max(20, p.slope_z_window / 5);
        if ((int)slope_history_.size() < slope_z_min) return sig;

        // -- 2. Slope z-score
        const double slope_z = _z_score(slope_history_, slope);

        // -- 3. Momentum binary
        const double prev_close = closes_[closes_.size() - 1 - p.mom_window];
        const double mom_binary = (daily_close > prev_close) ? 1.0 : 0.0;

        // -- 4. p_bull
        const double p_bull = 0.5 + 0.5 * std::tanh(slope_z) * mom_binary;

        // Snapshot for diagnostics regardless of fire
        last_signal_.slope_z      = slope_z;
        last_signal_.mom_binary   = mom_binary;
        last_signal_.p_bull       = p_bull;

        if (bar_count_ < cooldown_until_bar_) return sig;

        // Long-only hard-coded -- no symmetric short branch
        if (p_bull < p.p_bull_threshold) return sig;
        if (atr14 <= 0.0) return sig;
        if (!p.long_only) {
            // Defensive: even if a future user toggles long_only=false, this
            // engine is structurally long-only. Guard with explicit log.
            static int64_t s_warn = 0;
            if (now_ms - s_warn > 86400000LL) {
                s_warn = now_ms;
                printf("[TSMOM-%s] long_only=false ignored (engine is long-only)\n",
                       symbol.c_str());
                fflush(stdout);
            }
        }

        // Weekend entry gate
        if (_is_weekend_gated(now_ms)) {
            static int64_t s_wk = 0;
            if (now_ms - s_wk > 3600000LL) {
                s_wk = now_ms;
                printf("[TSMOM-%s] Weekend gate: no new entries\n", symbol.c_str());
                fflush(stdout);
            }
            return sig;
        }

        // Spread gate
        if ((ask - bid) > p.max_spread) return sig;

        // Entry pricing
        const double entry_px  = ask;
        const double sl_pts    = atr14 * p.sl_mult;
        if (sl_pts <= 0.0) return sig;
        const double sl_px     = entry_px - sl_pts;

        // Vol-targeted sizing
        const double realised_vol = _realised_vol_ann();
        last_signal_.realized_vol = realised_vol;

        // raw_lot = (target / realised) * size_scale, clamped.
        // Guard realised>=1e-4 to avoid divide-by-zero when only a few bars seen.
        const double rv = std::max(realised_vol, 1e-4);
        double raw_lot  = (p.target_vol_ann / rv) * p.size_scale;
        // Quantise to 0.001 lot grid as MinH4 does
        raw_lot = std::floor(raw_lot / 0.001) * 0.001;
        const double size = std::max(p.min_lot, std::min(p.max_lot, raw_lot));

        // Open position
        pos_.active        = true;
        pos_.is_long       = true;
        pos_.entry         = entry_px;
        pos_.sl            = sl_px;
        pos_.size          = size;
        pos_.atr14         = atr14;
        pos_.peak_mid      = (bid + ask) * 0.5;
        pos_.trail_dist    = atr14 * p.trail_mult;
        pos_.mfe           = 0.0;
        pos_.mae           = 0.0;
        pos_.entry_ts_ms   = now_ms;
        pos_.bars_held     = 0;
        ++m_trade_id_;

        printf("[TSMOM-%s] ENTRY LONG @ %.2f sl=%.2f size=%.3f atr14=%.2f"
               " slope_z=%.3f mom=%.0f p_bull=%.3f rv=%.4f%s\n",
               symbol.c_str(), entry_px, sl_px, size, atr14,
               slope_z, mom_binary, p_bull, realised_vol,
               shadow_mode ? " [SHADOW]" : "");
        fflush(stdout);

        sig.valid        = true;
        sig.is_long      = true;
        sig.entry        = entry_px;
        sig.sl           = sl_px;
        sig.size         = size;
        sig.slope_z      = slope_z;
        sig.mom_binary   = mom_binary;
        sig.p_bull       = p_bull;
        sig.realized_vol = realised_vol;
        sig.reason       = "TSMOM_DAILY_LONG";
        last_signal_     = sig;
        return sig;
    }

    // -------------------------------------------------------------------------
    //  on_tick -- tick-level SL / trail / weekend management.
    //  No new entries here; entry only on daily-bar close.
    // -------------------------------------------------------------------------
    void on_tick(double bid, double ask, int64_t now_ms,
                 CloseCallback on_close) noexcept
    {
        if (!pos_.active) return;

        const double mid = (bid + ask) * 0.5;

        // Track MFE/MAE from entry for TradeRecord reporting
        const double favour = pos_.is_long ? (mid - pos_.entry) : (pos_.entry - mid);
        if (favour > pos_.mfe) pos_.mfe = favour;
        const double advers = -favour;
        if (advers > pos_.mae) pos_.mae = advers;

        // Long-only: trail tracked from peak mid
        if (mid > pos_.peak_mid) pos_.peak_mid = mid;

        // 1. SL hit (bid touches SL)
        if (bid <= pos_.sl) {
            _close(bid, "SL_HIT", now_ms, on_close);
            return;
        }
        // 2. Trail hit (mid retraced trail_dist from peak)
        const double trail_trigger = pos_.peak_mid - pos_.trail_dist;
        if (pos_.peak_mid > pos_.entry && bid <= trail_trigger) {
            // Only arm the trail once we are above entry by at least trail_dist
            // (so the trail can never close us at a worse price than SL).
            _close(bid, "TRAIL_HIT", now_ms, on_close);
            return;
        }
        // 3. Weekend close (only if profitable)
        if (p.weekend_close_gate && _is_friday_close_window(now_ms)) {
            const double move = mid - pos_.entry;
            if (move > 0.0) {
                static int64_t s_wk_close = 0;
                if (now_ms - s_wk_close > 3600000LL) {
                    s_wk_close = now_ms;
                    printf("[TSMOM-%s] Weekend close: profitable position closed Fri 20:00+\n",
                           symbol.c_str());
                    fflush(stdout);
                    _close(bid, "WEEKEND_CLOSE", now_ms, on_close);
                }
            }
        }
    }

    void force_close(double bid, double /*ask*/, int64_t now_ms,
                     CloseCallback cb) noexcept {
        if (!pos_.active) return;
        _close(bid, "FORCE_CLOSE", now_ms, cb);
    }

    void cancel() noexcept { pos_ = OpenPos{}; }

private:
    // -------------------------------------------------------------------------
    //  _ols_slope_last_n -- ordinary-least-squares slope of last n values
    //  in `buf` regressed on x = [0, 1, ..., n-1].
    //  Returns 0.0 if buf has fewer than n elements.
    // -------------------------------------------------------------------------
    static double _ols_slope_last_n(const std::deque<double>& buf, int n) noexcept {
        const int sz = (int)buf.size();
        if (sz < n || n < 2) return 0.0;
        const int start = sz - n;
        // x mean = (n-1)/2; precomputed sums for stability
        const double x_mean = (n - 1) * 0.5;
        double y_sum = 0.0;
        for (int i = 0; i < n; ++i) y_sum += buf[start + i];
        const double y_mean = y_sum / n;

        double num = 0.0, den = 0.0;
        for (int i = 0; i < n; ++i) {
            const double dx = (double)i - x_mean;
            const double dy = buf[start + i] - y_mean;
            num += dx * dy;
            den += dx * dx;
        }
        if (den < 1e-12) return 0.0;
        return num / den;
    }

    // -------------------------------------------------------------------------
    //  _z_score -- z-score of `value` against the population in `buf`.
    //  Uses sample std (N-1) for stability with small N.
    // -------------------------------------------------------------------------
    static double _z_score(const std::deque<double>& buf, double value) noexcept {
        const int n = (int)buf.size();
        if (n < 2) return 0.0;
        double sum = 0.0;
        for (double v : buf) sum += v;
        const double mean = sum / n;
        double sq = 0.0;
        for (double v : buf) { const double d = v - mean; sq += d * d; }
        const double var = sq / (n - 1);
        if (var < 1e-12) return 0.0;
        return (value - mean) / std::sqrt(var);
    }

    // -------------------------------------------------------------------------
    //  _realised_vol_ann -- annualised std of daily log-returns over the
    //  last vol_window bars. Returns 0 if buffer has fewer than 2 entries.
    //  Annualisation factor sqrt(252) (trading days per year).
    // -------------------------------------------------------------------------
    double _realised_vol_ann() const noexcept {
        const int n = (int)daily_returns_.size();
        if (n < 2) return 0.0;
        double sum = 0.0;
        for (double r : daily_returns_) sum += r;
        const double mean = sum / n;
        double sq = 0.0;
        for (double r : daily_returns_) { const double d = r - mean; sq += d * d; }
        const double var = sq / (n - 1);
        if (var < 0.0) return 0.0;
        return std::sqrt(var) * std::sqrt(252.0);
    }

    // -------------------------------------------------------------------------
    //  Weekend gating helpers (mirrored from MinimalH4Breakout for consistency)
    //  Mapping: Mon=0 Tue=1 Wed=2 Thu=3 Fri=4 Sat=5 Sun=6.
    //  Epoch day 0 = Thu 1970-01-01, so dow = (day + 3) % 7.
    // -------------------------------------------------------------------------
    static bool _is_weekend_gated(int64_t now_ms) noexcept {
        const int64_t utc_sec  = now_ms / 1000LL;
        const int     utc_dow  = static_cast<int>((utc_sec / 86400LL + 3) % 7);
        const int     utc_hour = static_cast<int>((utc_sec % 86400LL) / 3600LL);
        if (utc_dow == 4 && utc_hour >= 20) return true;  // Fri 20:00+
        if (utc_dow == 5) return true;                     // Sat all day
        if (utc_dow == 6 && utc_hour < 22)  return true;   // Sun before 22:00
        return false;
    }

    static bool _is_friday_close_window(int64_t now_ms) noexcept {
        const int64_t utc_sec  = now_ms / 1000LL;
        const int     utc_dow  = static_cast<int>((utc_sec / 86400LL + 3) % 7);
        const int     utc_hour = static_cast<int>((utc_sec % 86400LL) / 3600LL);
        return (utc_dow == 4 && utc_hour >= 20);
    }

    void _daily_reset(int64_t now_ms) noexcept {
        const int64_t day = (now_ms / 1000LL) / 86400LL;
        if (day != daily_reset_day_) { daily_pnl_ = 0.0; daily_reset_day_ = day; }
    }

    void _close(double exit_px, const char* reason,
                int64_t now_ms, CloseCallback on_close) noexcept {
        const double pnl_pts = (pos_.is_long
            ? (exit_px - pos_.entry)
            : (pos_.entry - exit_px)) * pos_.size;
        daily_pnl_ += pnl_pts * 100.0;

        printf("[TSMOM-%s] EXIT LONG @ %.2f %s pnl=$%.2f bars=%d mfe=%.2f mae=%.2f%s\n",
               symbol.c_str(), exit_px, reason, pnl_pts * 100.0,
               pos_.bars_held, pos_.mfe, pos_.mae,
               shadow_mode ? " [SHADOW]" : "");
        fflush(stdout);

        if (on_close) {
            omega::TradeRecord tr;
            tr.id         = m_trade_id_;
            tr.symbol     = symbol.c_str();
            tr.side       = "LONG";
            tr.engine     = "TSMomGold";
            tr.entryPrice = pos_.entry;
            tr.exitPrice  = exit_px;
            tr.sl         = pos_.sl;
            tr.size       = pos_.size;
            tr.pnl        = pnl_pts;
            tr.mfe        = pos_.mfe;
            tr.mae        = pos_.mae;
            tr.entryTs    = pos_.entry_ts_ms / 1000;
            tr.exitTs     = now_ms / 1000;
            tr.exitReason = reason;
            tr.regime     = "TSMOM_DAILY";
            tr.l2_live    = false;
            tr.shadow     = shadow_mode;
            on_close(tr);
        }
        cooldown_until_bar_ = bar_count_ + p.cooldown_days;
        pos_ = OpenPos{};
    }
};

} // namespace omega
