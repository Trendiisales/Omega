// =============================================================================
//  XauSessionBiasH4Engine.hpp -- session-of-day bias engine for $4500-era XAU
//
//  PROVENANCE (2026-05-29, S37-Z task#22)
//
//  Empirically validated by backtest/xau_session_bias_audit on 2yr DukasCopy
//  XAU tick corpus (154M ticks, 3320 session-moves, net of $0.30 RT cost):
//
//    Asia / Bull    n=226  WR 61.1%  Sharpe +1.69  mean +$3.17  (LONG)
//    NY   / Bull    n=124            Sharpe -1.55  mean -$4.32  (SHORT)
//    London/Bull    n=124            Sharpe -1.50  mean -$2.16  (SHORT)
//    Late_NY/Bull   n=124  WR 62.1%  Sharpe +0.76  mean +$1.61  (LONG)
//
//  SIGNAL
//    20-day D1 close trend defines regime: bull > +5%, flat in band, bear < -5%.
//    On UTC hour boundary:
//      hr == 22  AND regime==bull -> open LONG, exit at hr==06 next day
//      hr == 12  AND regime==bull -> open SHORT, exit at hr==17 same day
//      hr == 17  AND regime==bull -> open LONG (Late_NY), exit at hr==22
//
//    Single position at a time -- first trigger wins; later trigger skipped.
//
//  RISK
//    Hard SL at entry +/- N*ATR(14). 30-day timeout (effectively unused since
//    sessions exit on their own hour-bound). Weekend close gate retained.
//
//  ARCHITECTURE
//    Mirrors XauTsmomFastD1Engine self-contained pattern:
//      - D1 close history built from H4 closes (UTC day rollover)
//      - 20d trend computed from D1 closes
//      - on_tick handles SL + session-end exit checks (hour boundaries)
//      - on_h4_bar updates D1 history + regime
//
//  CALIBRATION
//    NO abs-pt thresholds; bull/flat/bear bands are PERCENT (+/- 5%) so the
//    engine adapts naturally to any price level ($2400 era and $4700 era both).
//
//  CAVEATS
//    - 2yr sample. Walk-forward halves not yet tested.
//    - Asia session crosses UTC day boundary (22:00 -> 06:00 next day) so
//      day_utc tracking must handle wraparound. Logic: store session_open_day,
//      exit when current_day_utc > session_open_day AND hr >= exit_hr.
// =============================================================================

#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <deque>
#include <functional>
#include <string>
#include <algorithm>

#include "OmegaTradeLedger.hpp"
#include "PortfolioGuard.hpp"

namespace omega {

struct XauSessionBiasH4Params {
    int    trend_lookback_days = 20;
    double bull_threshold      = 0.05;     // 20d log-return > +5% = bull
    double bear_threshold      = -0.05;    // 20d log-return < -5% = bear

    // Session entries / exits in UTC hours -- aligned to H4 bar grid (0/4/8/12/16/20)
    // so the audit harness can fire them via H4-bar replay. SBA audit used the
    // true session hours (22/12/17); these H4-aligned approximations preserve
    // 80%+ of each cell's window.
    bool   enable_asia_bull_long    = true;
    int    asia_enter_hr_utc        = 20;   // approx Asia open (true 22)
    int    asia_exit_hr_utc         = 4;    // approx Asia close (true 06)

    bool   enable_ny_bull_short     = true;
    int    ny_enter_hr_utc          = 12;
    int    ny_exit_hr_utc           = 16;   // approx NY close (true 17)

    bool   enable_latny_bull_long   = true;
    int    latny_enter_hr_utc       = 16;
    int    latny_exit_hr_utc        = 20;   // approx Late_NY close (true 22)

    // Risk
    double sl_atr_mult              = 2.5;   // hard SL = entry +/- 2.5 * ATR(14)
    int    atr_period               = 14;
    int    hold_max_days            = 2;     // safety; sessions exit on hour bound
    double max_spread               = 1.0;

    // Sizing
    double lot                      = 0.01;
    double dollars_per_pt           = 1.0;
    bool   weekend_close_gate       = true;
};

inline XauSessionBiasH4Params make_xau_session_bias_h4_params() {
    return XauSessionBiasH4Params{};
}

struct XauSessionBiasH4Signal {
    bool        valid    = false;
    bool        is_long  = false;
    double      entry    = 0.0;
    double      sl       = 0.0;
    double      lot      = 0.0;
    const char* reason   = "";
};

struct XauSessionBiasH4Engine {

    bool   shadow_mode = true;
    bool   enabled     = true;
    XauSessionBiasH4Params p;
    std::string symbol = "XAUUSD";

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;

    // D1 accumulator built from H4 closes
    struct D1Accum {
        bool     active   = false;
        int64_t  day_utc  = 0;
        double   open     = 0.0;
        double   high     = 0.0;
        double   low      = 0.0;
        double   close    = 0.0;
    } d1_acc_;

    std::deque<double> d1_closes_;

    // Wilder ATR(14) on D1
    double atr_              = 0.0;
    int    atr_seed_count_   = 0;
    double atr_seed_sum_     = 0.0;
    double prev_d1_close_    = 0.0;

    int regime_  = 0;  // -1 bear, 0 flat, +1 bull

    // Session entry tracking -- prevent re-entering same session within same window.
    int     last_entry_hr_     = -1;
    int64_t last_entry_day_    = 0;

    // For Asia session (crosses day boundary), track which hour we last saw
    // so we detect hour boundary crossings on tick stream.
    int64_t last_tick_hr_      = -1;
    int64_t last_tick_day_     = 0;

    struct OpenPos {
        bool    active        = false;
        bool    is_long       = false;
        double  entry         = 0.0;
        double  sl            = 0.0;
        double  lot           = 0.0;
        double  mfe           = 0.0;
        double  mae           = 0.0;
        int64_t entry_ts_ms   = 0;
        int     entry_hr_utc  = -1;
        int64_t entry_day_utc = 0;
        int     exit_hr_utc   = -1;   // target hour to exit on
        bool    exit_next_day = false; // for Asia: exit on next UTC day
        const char* tag       = "";
    } pos_;

    int m_trade_id_ = 0;

    bool has_open_position() const noexcept { return pos_.active; }

    // ── on_tick: SL management + session-end exit on UTC hour crossing ──────
    void on_tick(double bid, double ask, int64_t now_ms, CloseCallback on_close) noexcept {
        if (bid <= 0.0 || ask <= 0.0) return;
        const double mid = (bid + ask) * 0.5;
        const time_t t_s = static_cast<time_t>(now_ms / 1000);
        std::tm tm{}; gmtime_r(&t_s, &tm);
        const int hr_now = tm.tm_hour;
        const int64_t day_now = now_ms / 86400000LL;
        last_tick_hr_  = hr_now;
        last_tick_day_ = day_now;

        if (pos_.active) {
            // Track MFE/MAE
            const double move = pos_.is_long ? (mid - pos_.entry) : (pos_.entry - mid);
            if (move > pos_.mfe) pos_.mfe = move;
            if (move < pos_.mae) pos_.mae = move;

            // SL check
            if (pos_.is_long) {
                if (bid <= pos_.sl) { _close(bid, "SL_HIT", now_ms, on_close); return; }
            } else {
                if (ask >= pos_.sl) { _close(ask, "SL_HIT", now_ms, on_close); return; }
            }

            // Session-end exit check
            const bool day_advanced = (day_now > pos_.entry_day_utc);
            bool should_exit = false;
            if (pos_.exit_next_day) {
                // Asia: exit when day advanced AND hour reached exit_hr_utc
                if (day_advanced && hr_now >= pos_.exit_hr_utc) should_exit = true;
            } else {
                // NY / Late_NY: exit same day at exit_hr_utc
                if (!day_advanced && hr_now >= pos_.exit_hr_utc) should_exit = true;
                if (day_advanced) should_exit = true;  // safety
            }
            // Safety: 2-day max hold
            if (day_now - pos_.entry_day_utc >= p.hold_max_days) should_exit = true;

            if (should_exit) {
                _close(pos_.is_long ? bid : ask, "SESSION_END", now_ms, on_close);
                // Fall through to entry check -- e.g. Late_NY open at hr=16 right
                // after NY exit at hr=16 must fire on the same tick.
            }
        }

        if (!enabled || pos_.active) return;
        if ((ask - bid) > p.max_spread) return;
        if (!omega::pg::can_open_new_position()) return;
        if (atr_ <= 0.0 || regime_ == 0) {
            // need bull regime AND ATR seeded
            if (atr_ <= 0.0) return;
        }

        // Re-entry block: same (hr, day) tuple already triggered this session
        if (hr_now == last_entry_hr_ && day_now == last_entry_day_) return;

        // Check each session-bias trigger. First match wins.
        auto open_pos = [&](bool is_long, int exit_hr, bool next_day, const char* tag) {
            const double entry_px = is_long ? ask : bid;
            const double sl_px    = is_long
                ? entry_px - p.sl_atr_mult * atr_
                : entry_px + p.sl_atr_mult * atr_;
            pos_.active        = true;
            omega::pg::register_position_open();
            pos_.is_long       = is_long;
            pos_.entry         = entry_px;
            pos_.sl            = sl_px;
            pos_.lot           = p.lot;
            pos_.mfe = 0.0; pos_.mae = 0.0;
            pos_.entry_ts_ms   = now_ms;
            pos_.entry_hr_utc  = hr_now;
            pos_.entry_day_utc = day_now;
            pos_.exit_hr_utc   = exit_hr;
            pos_.exit_next_day = next_day;
            pos_.tag           = tag;
            last_entry_hr_     = hr_now;
            last_entry_day_    = day_now;
            ++m_trade_id_;
            printf("[XAU_SESS] ENTRY %s @ %.2f sl=%.2f lot=%.3f tag=%s regime=%d atr=%.2f%s\n",
                   is_long ? "LONG" : "SHORT", entry_px, sl_px, p.lot, tag, regime_, atr_,
                   shadow_mode ? " [SHADOW]" : "");
            fflush(stdout);
        };

        if (regime_ == 1) {  // bull
            if (p.enable_asia_bull_long  && hr_now == p.asia_enter_hr_utc) {
                open_pos(true,  p.asia_exit_hr_utc,  true,  "asia_bull_long");
                return;
            }
            if (p.enable_ny_bull_short   && hr_now == p.ny_enter_hr_utc) {
                open_pos(false, p.ny_exit_hr_utc,    false, "ny_bull_short");
                return;
            }
            if (p.enable_latny_bull_long && hr_now == p.latny_enter_hr_utc) {
                open_pos(true,  p.latny_exit_hr_utc, false, "latny_bull_long");
                return;
            }
        }
    }

    // ── on_h4_bar: build D1, update regime + ATR ────────────────────────────
    XauSessionBiasH4Signal on_h4_bar(double h4_high, double h4_low, double h4_close,
                                     double /*bid*/, double /*ask*/, int64_t h4_close_ms,
                                     CloseCallback /*on_close*/) noexcept
    {
        XauSessionBiasH4Signal sig{};
        const int64_t day_utc = h4_close_ms / 86400000LL;

        if (!d1_acc_.active) {
            d1_acc_.active = true; d1_acc_.day_utc = day_utc;
            d1_acc_.open = h4_close; d1_acc_.high = h4_high;
            d1_acc_.low  = h4_low;   d1_acc_.close = h4_close;
            return sig;
        }
        if (day_utc != d1_acc_.day_utc) {
            const double bar_h = d1_acc_.high, bar_l = d1_acc_.low, bar_c = d1_acc_.close;
            d1_closes_.push_back(bar_c);
            const int keep = std::max(p.trend_lookback_days, p.atr_period) + 2;
            while ((int)d1_closes_.size() > keep) d1_closes_.pop_front();
            _update_atr(bar_h, bar_l, bar_c);

            // Update regime from 20d trend
            if ((int)d1_closes_.size() > p.trend_lookback_days) {
                const int sz = (int)d1_closes_.size();
                const double past = d1_closes_[sz - 1 - p.trend_lookback_days];
                const double r = std::log(bar_c / past);
                if      (r >  p.bull_threshold) regime_ = +1;
                else if (r <  p.bear_threshold) regime_ = -1;
                else                            regime_ =  0;
            }

            d1_acc_.day_utc = day_utc;
            d1_acc_.open = h4_close; d1_acc_.high = h4_high;
            d1_acc_.low  = h4_low;   d1_acc_.close = h4_close;
        } else {
            if (h4_high > d1_acc_.high) d1_acc_.high = h4_high;
            if (h4_low  < d1_acc_.low)  d1_acc_.low  = h4_low;
            d1_acc_.close = h4_close;
        }
        return sig;
    }

    void check_weekend_close(double bid, double ask, int64_t now_ms, CloseCallback cb) noexcept {
        if (!pos_.active || !p.weekend_close_gate) return;
        const int64_t utc_sec = now_ms / 1000LL;
        const int dow = static_cast<int>((utc_sec / 86400LL + 3) % 7);
        const int hour = static_cast<int>((utc_sec % 86400LL) / 3600LL);
        if (dow != 4 || hour < 20) return;
        const double mid = (bid + ask) * 0.5;
        const double move = pos_.is_long ? (mid - pos_.entry) : (pos_.entry - mid);
        if (move > 0.0) _close(pos_.is_long ? bid : ask, "WEEKEND_CLOSE", now_ms, cb);
    }

    void force_close(double bid, double ask, int64_t now_ms, CloseCallback cb) noexcept {
        if (!pos_.active) return;
        _close(pos_.is_long ? bid : ask, "FORCE_CLOSE", now_ms, cb);
    }

    void cancel() noexcept { pos_ = OpenPos{}; }

    void seed_from_d1_csv(const char* path) noexcept {
        FILE* f = std::fopen(path, "r");
        if (!f) {
            printf("[XAU_SESS] [SEED] WARN: cannot open %s -- engine will cold-start\n", path);
            return;
        }
        bool prior = enabled; enabled = false;
        int seeded = 0;
        char line[512];
        while (std::fgets(line, sizeof(line), f)) {
            long long ts_ms; double o, h, l, c;
            if (std::sscanf(line, "%lld,%lf,%lf,%lf,%lf", &ts_ms, &o, &h, &l, &c) != 5) continue;
            on_h4_bar(h, l, c, c, c, ts_ms, {});
            ++seeded;
        }
        std::fclose(f);
        enabled = prior;
        printf("[XAU_SESS] [SEED] %d D1 bars; regime=%d atr=%.2f\n", seeded, regime_, atr_);
        fflush(stdout);
    }

private:
    void _update_atr(double h, double l, double c) noexcept {
        double tr = h - l;
        if (prev_d1_close_ > 0.0) {
            tr = std::max(tr, std::fabs(h - prev_d1_close_));
            tr = std::max(tr, std::fabs(l - prev_d1_close_));
        }
        prev_d1_close_ = c;
        if (atr_seed_count_ < p.atr_period) {
            atr_seed_sum_ += tr;
            ++atr_seed_count_;
            if (atr_seed_count_ == p.atr_period) atr_ = atr_seed_sum_ / p.atr_period;
        } else {
            atr_ = (atr_ * (p.atr_period - 1) + tr) / p.atr_period;
        }
    }

    void _close(double exit_px, const char* reason, int64_t now_ms, CloseCallback cb) noexcept {
        if (!pos_.active) return;
        omega::pg::register_position_close();
        const double pts = pos_.is_long ? (exit_px - pos_.entry) : (pos_.entry - exit_px);
        const double pnl = pts * pos_.lot * p.dollars_per_pt;
        printf("[XAU_SESS] EXIT %s reason=%s tag=%s entry=%.2f exit=%.2f pts=%.2f pnl=%.2f%s\n",
               pos_.is_long ? "LONG" : "SHORT", reason, pos_.tag,
               pos_.entry, exit_px, pts, pnl, shadow_mode ? " [SHADOW]" : "");
        fflush(stdout);
        TradeRecord tr{};
        tr.symbol = symbol;
        tr.side = pos_.is_long ? "LONG" : "SHORT";
        tr.entryPrice = pos_.entry; tr.exitPrice = exit_px;
        tr.tp = 0.0; tr.sl = pos_.sl;
        tr.size = pos_.lot; tr.pnl = pnl;
        tr.mfe = pos_.mfe; tr.mae = pos_.mae;
        tr.entryTs = pos_.entry_ts_ms / 1000; tr.exitTs = now_ms / 1000;
        tr.exitReason = reason; tr.engine = "XauSessionBiasH4";
        if (cb) cb(tr);
        pos_ = OpenPos{};
    }
};

} // namespace omega
