#pragma once
// =============================================================================
// BarState.hpp -- per-bar accumulator + all rolling trailing state.
//
// LEAKAGE-CRITICAL DESIGN:
// The features emitted on the close-row of bar t MUST be computable from
// information available before bar t+1 starts. Equivalently:
//   * Bar-internal features (open/high/low/close/range/etc) come from ticks
//     within bar t and are sealed at bar close.
//   * Trailing features (EMA, RSI, ATR, range_20bar, vol, etc) are computed
//     from bars 1..t (NOT bars 1..t-1). It is acceptable to use bar t's own
//     close in these because the close is sealed at the same instant the
//     features are written. The point is they cannot use t+1.
//
// To make this airtight, BarState exposes two phases:
//
//   1. on_tick(bid, ask, ts_ms)
//        Updates the in-progress bar's accumulators and notes the tick.
//        Does NOT touch trailing state.
//
//   2. close_bar(out_features)
//        Called when a tick crosses a minute boundary. Steps:
//          a. Finalise this bar's tick stats (open/high/low/close/spread).
//          b. Snapshot the *current* trailing rings -> emit feature row.
//             At this point, trailing = "up to bar t-1".
//          c. Update trailing rings with bar t's close (next bar will see them).
//          d. Reset the in-progress accumulators.
//
//        Returns the closed bar's mid price (used by the extractor as the
//        forward-return anchor) and the median spread (used as cost basis).
//
// This ordering is what the leakage test verifies: at step (b), trailing
// features are only computed from bars 1..t-1.
// =============================================================================

#include "PanelSchema.hpp"
#include "CivilTime.hpp"
#include "RollingWindow.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace edgefinder {

class BarState {
public:
    // Result of closing a bar: passed back to extractor.
    struct CloseResult {
        bool   emitted;         // false on the very first tick (no prior bar)
        double close_mid;       // mid price at bar close (for fwd returns)
        double spread_median;   // median spread in pts (for cost basis)
        int64_t bar_close_ms;   // epoch ms at bar close boundary
    };

    BarState() { reset_all(); }

    // Push one tick. ts_ms must be monotonically non-decreasing.
    // Returns CloseResult{emitted=true, ...} when this tick crossed a minute
    // boundary and a previous bar was therefore closed (and feature row
    // populated into `out`).
    CloseResult on_tick(double bid, double ask, int64_t ts_ms,
                        PanelRow& out) noexcept
    {
        CloseResult r{false, 0.0, 0.0, 0};

        const double mid    = 0.5 * (bid + ask);
        const double spread = ask - bid;
        const int64_t bar_t = (ts_ms / 60000LL) * 60000LL;

        if (!have_bar_) {
            // First tick ever: just initialise the in-progress bar.
            init_bar(bar_t, mid, spread, ts_ms);
            have_bar_ = true;
            return r;
        }

        if (bar_t != cur_bar_open_ms_) {
            // We've crossed at least one minute boundary. Close the current
            // bar (and any empty bars between it and bar_t -- which would only
            // happen on data gaps; we don't fabricate ticks across gaps).
            r = close_current_bar(out);
            init_bar(bar_t, mid, spread, ts_ms);
            return r;
        }

        // Same bar: accumulate.
        if (mid > cur_high_) cur_high_ = mid;
        if (mid < cur_low_)  cur_low_  = mid;
        cur_close_     = mid;
        cur_close_bid_ = bid;
        cur_close_ask_ = ask;
        cur_spreads_.push_back(spread);
        ++cur_tick_count_;

        // Tick-level rolling state needed for session VWAP and Asian range:
        // these update intra-bar so we don't lose granularity on h=06:59 -> 07:00 boundary.
        update_session_intratick(mid, ts_ms);
        update_daily_intratick(mid, ts_ms);
        return r;
    }

    // Force-close the in-progress bar (called at end of input).
    bool finalise(PanelRow& out) noexcept {
        if (!have_bar_) return false;
        CloseResult r = close_current_bar(out);
        return r.emitted;
    }

    int64_t bars_emitted() const noexcept { return bars_emitted_; }

private:
    // ---- in-progress bar (resets each minute) ----
    bool    have_bar_ = false;
    int64_t cur_bar_open_ms_ = 0;
    double  cur_open_ = 0.0, cur_high_ = 0.0, cur_low_ = 0.0, cur_close_ = 0.0;
    double  cur_close_bid_ = 0.0, cur_close_ask_ = 0.0;
    int     cur_tick_count_ = 0;
    std::vector<double> cur_spreads_;  // sized small (~150 ticks/min typical)

    // ---- prior bars used at close-time for inside/outside/NR4/NR7 ----
    Ring<double, 8> last_high_;
    Ring<double, 8> last_low_;
    Ring<double, 8> last_close_;
    Ring<double, 8> last_open_;
    Ring<double, 8> last_range_;

    // ---- trailing series of bar closes (drives EMA, RSI, ATR, range, vol) ----
    // Updated AFTER we emit feature row. Rings sized power-of-2 >= max window.
    Ring<double, 64>     close_buf64_;     // for ret_60 / vol60 / etc.
    Ring<double, 64>     range_buf64_;     // not strictly used but cheap to keep
    MinMaxRing<double,32> range20_;        // 20-bar high/low (cap 32 = next pow2)
    RollingVar<64>        vol60_;          // 60-bar return stddev (cap 64)
    RollingVar<8>         vol5_;           // 5-bar return stddev  (cap 8)
    RollingMean<32>       bb_mean_;        // 20-bar mean for BB (cap 32)
    RollingVar<32>        bb_var_;         // 20-bar var  for BB
    RollingLinReg<8>      ema50_slope_;    // 5-bar slope (cap 8)

    // EMAs maintained as classic EMA (no warmup ring needed beyond a count).
    double ema9_   = 0.0; bool ema9_warm_   = false; int ema9_cnt_   = 0;
    double ema21_  = 0.0; bool ema21_warm_  = false; int ema21_cnt_  = 0;
    double ema50_  = 0.0; bool ema50_warm_  = false; int ema50_cnt_  = 0;
    double ema200_ = 0.0; bool ema200_warm_ = false; int ema200_cnt_ = 0;
    double prev_ema9_  = 0.0;  // for cross-detection
    double prev_ema50_ = 0.0;

    // Wilder ATR + RSI: maintained as classic Wilder smoothed values.
    double atr14_ = 0.0; bool atr14_warm_ = false; int atr14_cnt_ = 0;
    double atr50_ = 0.0; bool atr50_warm_ = false; int atr50_cnt_ = 0;
    double rsi_avg_gain_ = 0.0;
    double rsi_avg_loss_ = 0.0;
    bool   rsi_warm_     = false;
    int    rsi_cnt_      = 0;
    double prev_close_for_change_ = 0.0;  // bar t-1 close (for return into rsi/atr)

    // Streaks and recent-return memory.
    int     up_streak_   = 0;
    int     down_streak_ = 0;

    // Cross-detection memory (state on bar t-1 vs t).
    bool    prev_above_pdh_     = false;
    bool    prev_below_pdl_     = false;
    bool    prev_above_asian_hi_= false;
    bool    prev_below_asian_lo_= false;
    bool    prev_above_vwap_    = false;
    bool    prev_above_bb_upper_= false;
    bool    prev_below_bb_lower_= false;

    // ---- session state (resets at session boundary) ----
    Session current_session_   = Session::ASIAN;
    Session prev_close_session_= Session::ASIAN;
    double  session_open_      = 0.0;
    double  session_hi_        = 0.0;
    double  session_lo_        = 1e18;
    double  session_vwap_num_  = 0.0;
    double  session_vwap_den_  = 0.0;
    bool    session_seeded_    = false;

    // ---- daily state (resets at 00:00 UTC) ----
    int     last_yday_ = -1;
    double  today_hi_  = 0.0;
    double  today_lo_  = 1e18;
    double  pdh_       = 0.0;
    double  pdl_       = 0.0;
    bool    have_pd_   = false;
    double  asian_hi_  = 0.0;
    double  asian_lo_  = 1e18;
    bool    asian_built_ = false;

    int64_t bars_emitted_ = 0;
    double  prev_bar_close_ = 0.0;
    bool    have_prev_bar_close_ = false;

    // ---------- helpers ----------
    void reset_all() noexcept {
        last_high_.clear(); last_low_.clear(); last_close_.clear();
        last_open_.clear(); last_range_.clear();
        close_buf64_.clear(); range_buf64_.clear();
        range20_.clear();
        vol60_ = RollingVar<64>{};
        vol5_  = RollingVar<8>{};
        bb_mean_ = RollingMean<32>{};
        bb_var_  = RollingVar<32>{};
        ema50_slope_ = RollingLinReg<8>{};
        ema9_=ema21_=ema50_=ema200_=0.0;
        ema9_warm_=ema21_warm_=ema50_warm_=ema200_warm_=false;
        ema9_cnt_=ema21_cnt_=ema50_cnt_=ema200_cnt_=0;
        prev_ema9_=prev_ema50_=0.0;
        atr14_=atr50_=0.0;
        atr14_warm_=atr50_warm_=false;
        atr14_cnt_=atr50_cnt_=0;
        rsi_avg_gain_=rsi_avg_loss_=0.0;
        rsi_warm_=false; rsi_cnt_=0;
        prev_close_for_change_ = 0.0;
        up_streak_=down_streak_=0;
        bars_emitted_ = 0;
        prev_bar_close_ = 0.0;
        have_prev_bar_close_ = false;
    }

    void init_bar(int64_t bar_t, double mid, double spread, int64_t ts_ms) noexcept {
        cur_bar_open_ms_ = bar_t;
        cur_open_ = cur_high_ = cur_low_ = cur_close_ = mid;
        cur_close_bid_ = mid; cur_close_ask_ = mid;
        cur_tick_count_ = 1;
        cur_spreads_.clear();
        cur_spreads_.push_back(spread);
        update_session_intratick(mid, ts_ms);
        update_daily_intratick(mid, ts_ms);
    }

    void update_session_intratick(double mid, int64_t ts_ms) noexcept {
        const CivilTime t = civil_from_epoch_ms(ts_ms);
        const Session s   = classify_session_minute(t.mins_of_day);
        if (s != current_session_ || !session_seeded_) {
            current_session_   = s;
            session_open_      = mid;
            session_hi_        = mid;
            session_lo_        = mid;
            session_vwap_num_  = mid;
            session_vwap_den_  = 1.0;
            session_seeded_    = true;
        } else {
            if (mid > session_hi_) session_hi_ = mid;
            if (mid < session_lo_) session_lo_ = mid;
            session_vwap_num_ += mid;
            session_vwap_den_ += 1.0;
        }
    }

    void update_daily_intratick(double mid, int64_t ts_ms) noexcept {
        const CivilTime t = civil_from_epoch_ms(ts_ms);
        if (t.yday != last_yday_) {
            // Day boundary: archive yesterday's hi/lo, reset today's, drop Asian state.
            if (last_yday_ >= 0) {
                pdh_ = today_hi_;
                pdl_ = today_lo_;
                have_pd_ = (pdh_ > 0 && pdl_ < 1e17);
            }
            today_hi_ = mid; today_lo_ = mid;
            asian_hi_ = 0.0; asian_lo_ = 1e18; asian_built_ = false;
            last_yday_ = t.yday;
        } else {
            if (mid > today_hi_) today_hi_ = mid;
            if (mid < today_lo_) today_lo_ = mid;
        }
        // Asian range builds during 00:00..06:59 UTC (mins < 420).
        if (t.mins_of_day < 420) {
            if (mid > asian_hi_) asian_hi_ = mid;
            if (mid < asian_lo_) asian_lo_ = mid;
        } else {
            // First time we see a tick at >=07:00 UTC on a day where the range
            // accumulated something, mark it built. Stays built until next day.
            if (!asian_built_ && asian_hi_ > 0.0 && asian_lo_ < 1e17 && asian_hi_ > asian_lo_) {
                asian_built_ = true;
            }
        }
    }

    // ---------- the close-bar logic ----------
    CloseResult close_current_bar(PanelRow& out) noexcept {
        CloseResult r{};

        const double bar_open  = cur_open_;
        const double bar_high  = cur_high_;
        const double bar_low   = cur_low_;
        const double bar_close = cur_close_;
        const int    nticks    = cur_tick_count_;

        // Median spread: nth_element is fine, the bar is small.
        double sp_med = 0.0, sp_max = 0.0;
        if (!cur_spreads_.empty()) {
            sp_max = *std::max_element(cur_spreads_.begin(), cur_spreads_.end());
            std::vector<double>& v = cur_spreads_;
            const size_t k = v.size() / 2;
            std::nth_element(v.begin(), v.begin() + k, v.end());
            sp_med = v[k];
            if (v.size() % 2 == 0 && k > 0) {
                const double a = *std::max_element(v.begin(), v.begin() + k);
                sp_med = 0.5 * (a + sp_med);
            }
        }

        // ---- POPULATE feature row from PRIOR-bar trailing state ----
        // (close_buf64_ / EMAs / ATR / RSI / range20_ are still "up to t-1" here.)
        std::memset(&out, 0, sizeof(out));
        out.ts_close_ms = cur_bar_open_ms_ + 60000LL;  // bar close = open + 60s
        const CivilTime t = civil_from_epoch_ms(out.ts_close_ms - 1);
        out.utc_hour          = t.hour;
        out.utc_minute_of_day = t.mins_of_day;
        out.dow               = t.dow;
        out.dom               = t.dom;
        out.yday              = t.yday;
        out.session           = static_cast<uint8_t>(current_session_);

        // Bar-internal (all sealed by ticks within this bar).
        out.open               = bar_open;
        out.high               = bar_high;
        out.low                = bar_low;
        out.close              = bar_close;
        out.bar_range_pts      = bar_high - bar_low;
        out.bar_body_pts       = std::fabs(bar_close - bar_open);
        out.bar_upper_wick_pts = bar_high - std::max(bar_open, bar_close);
        out.bar_lower_wick_pts = std::min(bar_open, bar_close) - bar_low;
        out.bar_direction      = (bar_close > bar_open) ? 1 : (bar_close < bar_open) ? -1 : 0;
        out.tick_count         = nticks;
        out.spread_median_pts  = sp_med;
        out.spread_max_pts     = sp_max;

        // Trailing technicals (PRIOR state, not yet updated with this bar).
        out.ema_9          = ema9_warm_   ? ema9_   : std::nan("");
        out.ema_21         = ema21_warm_  ? ema21_  : std::nan("");
        out.ema_50         = ema50_warm_  ? ema50_  : std::nan("");
        out.ema_200        = ema200_warm_ ? ema200_ : std::nan("");
        out.ema_9_minus_50 = (ema9_warm_ && ema50_warm_) ? (ema9_ - ema50_) : std::nan("");
        out.ema_50_slope   = ema50_slope_.full() ? ema50_slope_.slope() : std::nan("");
        out.rsi_14         = rsi_warm_  ? compute_rsi() : std::nan("");
        out.atr_14         = atr14_warm_ ? atr14_ : std::nan("");
        out.atr_50         = atr50_warm_ ? atr50_ : std::nan("");

        if (range20_.size() >= 20) {
            out.range_20bar_hi = range20_.max();
            out.range_20bar_lo = range20_.min();
            const double rng = out.range_20bar_hi - out.range_20bar_lo;
            out.range_20bar_position = (rng > 0.0)
                ? (bar_close - out.range_20bar_lo) / rng
                : 0.5;
        } else {
            out.range_20bar_hi = std::nan("");
            out.range_20bar_lo = std::nan("");
            out.range_20bar_position = std::nan("");
        }

        if (bb_mean_.full() && bb_var_.full()) {
            const double mu  = bb_mean_.mean();
            const double sd  = bb_var_.stddev();
            out.bb_upper = mu + 2.0 * sd;
            out.bb_lower = mu - 2.0 * sd;
            const double width = out.bb_upper - out.bb_lower;
            out.bb_position = (width > 0.0) ? (bar_close - out.bb_lower) / width : 0.5;
        } else {
            out.bb_upper = std::nan("");
            out.bb_lower = std::nan("");
            out.bb_position = std::nan("");
        }

        out.vol_60bar_stddev = vol60_.size() >= 30 ? vol60_.stddev() : std::nan("");
        out.vol_5bar_stddev  = vol5_.full()        ? vol5_.stddev()  : std::nan("");
        out.vol_5_vs_60_ratio = (!std::isnan(out.vol_60bar_stddev) && out.vol_60bar_stddev > 0.0)
            ? (out.vol_5bar_stddev / out.vol_60bar_stddev) : std::nan("");

        // Session/structural.
        out.session_open_price     = session_open_;
        out.session_open_dist_pts  = bar_close - session_open_;
        out.session_high           = session_hi_;
        out.session_low            = session_lo_;
        out.session_range_pts      = session_hi_ - session_lo_;
        out.session_position       = (out.session_range_pts > 0.0)
            ? (bar_close - session_lo_) / out.session_range_pts : 0.5;
        out.vwap_session = (session_vwap_den_ > 0.0)
            ? (session_vwap_num_ / session_vwap_den_) : bar_close;
        out.vwap_dist_pts = bar_close - out.vwap_session;
        out.vwap_z = (!std::isnan(out.vol_60bar_stddev) && out.vol_60bar_stddev > 0.0)
            ? (out.vwap_dist_pts / out.vol_60bar_stddev) : std::nan("");

        out.pdh = have_pd_ ? pdh_ : std::nan("");
        out.pdl = have_pd_ ? pdl_ : std::nan("");
        out.above_pdh = (have_pd_ && bar_close > pdh_) ? 1 : 0;
        out.below_pdl = (have_pd_ && bar_close < pdl_) ? 1 : 0;
        out.dist_to_pdh_pts = have_pd_ ? (bar_close - pdh_) : std::nan("");
        out.dist_to_pdl_pts = have_pd_ ? (bar_close - pdl_) : std::nan("");

        out.asian_hi = (asian_hi_ > 0.0)    ? asian_hi_ : std::nan("");
        out.asian_lo = (asian_lo_ < 1e17)   ? asian_lo_ : std::nan("");
        out.asian_range_pts = (asian_built_) ? (asian_hi_ - asian_lo_) : std::nan("");
        out.asian_built     = asian_built_ ? 1 : 0;
        out.above_asian_hi  = (asian_built_ && bar_close > asian_hi_) ? 1 : 0;
        out.below_asian_lo  = (asian_built_ && bar_close < asian_lo_) ? 1 : 0;

        // Recent-move features from close_buf64_.
        const auto& cb = close_buf64_;
        auto get_ret = [&](size_t lookback) -> double {
            if (cb.size() < lookback) return std::nan("");
            // ret = bar_close - cb[size - lookback], where cb[0] is oldest.
            // bar_close is the JUST-CLOSED bar (not in cb yet).
            const double prev = cb[cb.size() - lookback];
            return bar_close - prev;
        };
        out.ret_1bar_pts  = get_ret(1);
        out.ret_5bar_pts  = get_ret(5);
        out.ret_15bar_pts = get_ret(15);
        out.ret_60bar_pts = get_ret(60);

        out.consecutive_up_bars   = up_streak_;
        out.consecutive_down_bars = down_streak_;

        // NR4/NR7: this-bar range vs last 3/6.
        {
            const double r = out.bar_range_pts;
            bool nr4 = false, nr7 = false;
            if (last_range_.size() >= 3) {
                nr4 = true;
                for (size_t i = 0; i < 3; ++i) if (last_range_[last_range_.size()-1-i] <= r) { nr4 = false; break; }
            }
            if (last_range_.size() >= 6) {
                nr7 = true;
                for (size_t i = 0; i < 6; ++i) if (last_range_[last_range_.size()-1-i] <= r) { nr7 = false; break; }
            }
            out.nr4 = nr4 ? 1 : 0;
            out.nr7 = nr7 ? 1 : 0;
        }

        // Inside / outside vs last bar.
        if (last_high_.size() >= 1) {
            const double ph = last_high_.back();
            const double pl = last_low_.back();
            out.inside_bar  = (bar_high <  ph && bar_low >  pl) ? 1 : 0;
            out.outside_bar = (bar_high >  ph && bar_low <  pl) ? 1 : 0;
        }

        out.gap_from_prev_close = have_prev_bar_close_
            ? (bar_open - prev_bar_close_) : std::nan("");

        // Transitions: prev-state vs this-bar-state.
        const bool above_pdh_now      = (out.above_pdh == 1);
        const bool below_pdl_now      = (out.below_pdl == 1);
        const bool above_asian_hi_now = (out.above_asian_hi == 1);
        const bool below_asian_lo_now = (out.below_asian_lo == 1);
        const bool above_vwap_now     = (bar_close > out.vwap_session);
        const bool above_bbu_now      = !std::isnan(out.bb_upper) && bar_close > out.bb_upper;
        const bool below_bbl_now      = !std::isnan(out.bb_lower) && bar_close < out.bb_lower;
        const bool ema9_above_50_now  = (ema9_warm_ && ema50_warm_) && (ema9_ > ema50_);
        const bool ema9_above_50_prev = (prev_ema9_ > 0.0 && prev_ema50_ > 0.0) && (prev_ema9_ > prev_ema50_);

        out.cross_above_pdh       = (above_pdh_now      && !prev_above_pdh_)      ? 1 : 0;
        out.cross_below_pdl       = (below_pdl_now      && !prev_below_pdl_)      ? 1 : 0;
        out.cross_above_asian_hi  = (above_asian_hi_now && !prev_above_asian_hi_) ? 1 : 0;
        out.cross_below_asian_lo  = (below_asian_lo_now && !prev_below_asian_lo_) ? 1 : 0;
        out.cross_above_vwap      = (above_vwap_now     && !prev_above_vwap_)     ? 1 : 0;
        out.cross_below_vwap      = (!above_vwap_now    &&  prev_above_vwap_)     ? 1 : 0;
        out.ema_9_50_bull_cross   = (ema9_above_50_now  && !ema9_above_50_prev)   ? 1 : 0;
        out.ema_9_50_bear_cross   = (!ema9_above_50_now &&  ema9_above_50_prev)   ? 1 : 0;
        out.enter_bb_upper        = (above_bbu_now      && !prev_above_bb_upper_) ? 1 : 0;
        out.enter_bb_lower        = (below_bbl_now      && !prev_below_bb_lower_) ? 1 : 0;

        // Forward-return cells start NaN; extractor will fill them.
        out.fwd_ret_1m_pts   = std::nan("");
        out.fwd_ret_5m_pts   = std::nan("");
        out.fwd_ret_15m_pts  = std::nan("");
        out.fwd_ret_60m_pts  = std::nan("");
        out.fwd_ret_240m_pts = std::nan("");
        out.first_touch_5m   = 0;
        out.first_touch_15m  = 0;
        out.first_touch_60m  = 0;
        for (int i = 0; i < N_BRACKETS; ++i) {
            out.fwd_bracket_pts[i]     = std::nan("");
            out.fwd_bracket_outcome[i] = 0;
        }

        // Warm-up flag: "all primary trailing series have enough samples for stats".
        const bool warm = ema50_warm_ && atr14_warm_ && rsi_warm_
                       && range20_.size() >= 20 && vol60_.size() >= 30;
        out.warmed_up = warm ? 1 : 0;
        out.fwd_complete = 0;

        // ---- NOW update trailing state with bar t's close ----
        update_trailing(bar_open, bar_high, bar_low, bar_close);

        // Record state used for next bar's transition computations.
        prev_above_pdh_      = above_pdh_now;
        prev_below_pdl_      = below_pdl_now;
        prev_above_asian_hi_ = above_asian_hi_now;
        prev_below_asian_lo_ = below_asian_lo_now;
        prev_above_vwap_     = above_vwap_now;
        prev_above_bb_upper_ = above_bbu_now;
        prev_below_bb_lower_ = below_bbl_now;
        prev_ema9_           = ema9_warm_  ? ema9_  : 0.0;
        prev_ema50_          = ema50_warm_ ? ema50_ : 0.0;

        prev_bar_close_      = bar_close;
        have_prev_bar_close_ = true;

        ++bars_emitted_;

        r.emitted       = true;
        r.close_mid     = bar_close;
        r.spread_median = sp_med;
        r.bar_close_ms  = out.ts_close_ms;
        return r;
    }

    static void ema_update(double& ema, bool& warm, int& cnt, int period, double v) noexcept {
        if (!warm) {
            ema = (ema * cnt + v) / (cnt + 1);
            ++cnt;
            if (cnt >= period) warm = true;
        } else {
            const double alpha = 2.0 / (period + 1.0);
            ema = v * alpha + ema * (1.0 - alpha);
        }
    }

    // Wilder smoothing used by both ATR and RSI: alpha = 1/N.
    static void wilder_update(double& smoothed, bool& warm, int& cnt, int N, double sample) noexcept {
        if (!warm) {
            smoothed = (smoothed * cnt + sample) / (cnt + 1);
            ++cnt;
            if (cnt >= N) warm = true;
        } else {
            smoothed = (smoothed * (N - 1) + sample) / static_cast<double>(N);
        }
    }

    double compute_rsi() const noexcept {
        if (rsi_avg_loss_ < 1e-12) return 100.0;
        const double rs  = rsi_avg_gain_ / rsi_avg_loss_;
        return 100.0 - (100.0 / (1.0 + rs));
    }

    void update_trailing(double bar_open, double bar_high, double bar_low, double bar_close) noexcept {
        // True range for ATR (using prior close where available).
        double tr;
        if (have_prev_bar_close_) {
            tr = std::max({ bar_high - bar_low,
                            std::fabs(bar_high - prev_close_for_change_),
                            std::fabs(bar_low  - prev_close_for_change_) });
        } else {
            tr = bar_high - bar_low;
        }
        wilder_update(atr14_, atr14_warm_, atr14_cnt_, 14, tr);
        wilder_update(atr50_, atr50_warm_, atr50_cnt_, 50, tr);

        // RSI gain/loss off close-to-close, classic Wilder.
        // Phase 1 (first 14 deltas): accumulate raw sum of gains/losses, store
        // running average. After 14 deltas, switch to Wilder smoothing.
        if (have_prev_bar_close_) {
            const double diff = bar_close - prev_close_for_change_;
            const double g = (diff > 0) ?  diff : 0.0;
            const double l = (diff < 0) ? -diff : 0.0;
            ++rsi_cnt_;
            if (!rsi_warm_) {
                // Simple running mean for first 14 deltas.
                rsi_avg_gain_ = (rsi_avg_gain_ * (rsi_cnt_ - 1) + g) / static_cast<double>(rsi_cnt_);
                rsi_avg_loss_ = (rsi_avg_loss_ * (rsi_cnt_ - 1) + l) / static_cast<double>(rsi_cnt_);
                if (rsi_cnt_ >= 14) rsi_warm_ = true;
            } else {
                // Wilder smoothing, alpha = 1/14
                rsi_avg_gain_ = (rsi_avg_gain_ * 13.0 + g) / 14.0;
                rsi_avg_loss_ = (rsi_avg_loss_ * 13.0 + l) / 14.0;
            }
        }

        // EMAs.
        ema_update(ema9_,   ema9_warm_,   ema9_cnt_,   9,   bar_close);
        ema_update(ema21_,  ema21_warm_,  ema21_cnt_,  21,  bar_close);
        ema_update(ema50_,  ema50_warm_,  ema50_cnt_,  50,  bar_close);
        ema_update(ema200_, ema200_warm_, ema200_cnt_, 200, bar_close);

        // Slope ring fed with current EMA50 so slope() returns slope of the last 5 EMA50s.
        if (ema50_warm_) ema50_slope_.push(ema50_);

        // Range / vol / BB rings.
        range20_.push_back(bar_close);
        const double prev_seed = have_prev_bar_close_ ? prev_close_for_change_ : bar_close;
        const double bar_ret = bar_close - prev_seed;
        vol60_.push(bar_ret);
        vol5_.push(bar_ret);
        bb_mean_.push(bar_close);
        bb_var_.push(bar_close);

        close_buf64_.push_back(bar_close);
        range_buf64_.push_back(bar_high - bar_low);

        // Last-N high/low/close/open/range buffers.
        last_high_.push_back(bar_high);
        last_low_.push_back(bar_low);
        last_close_.push_back(bar_close);
        last_open_.push_back(bar_open);
        last_range_.push_back(bar_high - bar_low);

        // Streaks.
        if (have_prev_bar_close_) {
            if (bar_close > prev_close_for_change_) {
                up_streak_ += 1;
                down_streak_ = 0;
            } else if (bar_close < prev_close_for_change_) {
                down_streak_ += 1;
                up_streak_ = 0;
            } else {
                up_streak_ = down_streak_ = 0;
            }
        }

        prev_close_for_change_ = bar_close;
    }
};

} // namespace edgefinder
