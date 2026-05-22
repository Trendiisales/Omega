// =============================================================================
//  GoldD1TrendState.hpp -- D1 EMA200 trend regime gate for XAUUSD shorts
//
//  PROVENANCE (2026-05-21)
//
//  After 2026-05-20 deploy showed XauTrendFollow2h InsideBar SHORT lost
//  -$52.31 in a strong gold uptrend (entry 4484, SL 4536, held 6h), added
//  this regime gate. Bidirectional engines query short_allowed()/long_allowed()
//  before firing direction-dependent entries.
//
//  Mechanism:
//    - Aggregates D1 bars from H4 close events (same pattern as existing
//      MinimalH4Breakout / XauTsmomFastD1 etc.)
//    - Maintains EMA200 of D1 closes + 10-day slope.
//    - short_allowed():  EMA200 slope <= -slope_threshold (clear downtrend)
//    - long_allowed():   EMA200 slope >= +slope_threshold (clear uptrend)
//    - neutral():        |slope| < slope_threshold (range -- both OK)
//
//  When regime is NEUTRAL, both directions are allowed (engines run normally).
//  When regime is UPTREND, shorts are blocked.
//  When regime is DOWNTREND, longs are blocked.
//
//  USAGE (from engine code):
//    if (sig.side == TradeSide::SHORT && !g_gold_d1_trend.short_allowed()) {
//        return;  // skip entry
//    }
//
//  WARM-SEED (per CLAUDE.md mandate):
//    seed_from_h4_csv("phase1/signal_discovery/warmup_XAUUSD_H4.csv");
//    Populates EMA200 + slope buffer so regime is queryable on first live tick.
// =============================================================================

#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <fstream>
#include <string>
#include <algorithm>
#include "SeedGuard.hpp"

namespace omega {

struct GoldD1TrendState {
    int    ema_period       = 200;
    int    slope_lookback   = 10;            // bars over which slope is measured
    double slope_threshold  = 0.0005;        // 0.05% of EMA per bar = "trending"

    // State
    double ema_              = 0.0;
    int    ema_count_        = 0;
    std::deque<double> ema_history_;         // last N EMAs for slope computation
    int    bar_count_        = 0;

    // D1 accumulator (built from H4 close events)
    bool     d1_active_   = false;
    int64_t  d1_day_utc_  = 0;
    double   d1_open_     = 0.0;
    double   d1_high_     = 0.0;
    double   d1_low_      = 0.0;
    double   d1_close_    = 0.0;

    // Trend classification
    enum class Regime { UNKNOWN, UPTREND, DOWNTREND, NEUTRAL };
    Regime current_regime_ = Regime::UNKNOWN;
    double current_slope_  = 0.0;

    bool short_allowed() const noexcept {
        return current_regime_ == Regime::DOWNTREND || current_regime_ == Regime::NEUTRAL
            || current_regime_ == Regime::UNKNOWN;  // unknown = don't block
    }
    bool long_allowed() const noexcept {
        return current_regime_ == Regime::UPTREND || current_regime_ == Regime::NEUTRAL
            || current_regime_ == Regime::UNKNOWN;
    }
    const char* regime_name() const noexcept {
        switch (current_regime_) {
            case Regime::UPTREND:   return "UPTREND";
            case Regime::DOWNTREND: return "DOWNTREND";
            case Regime::NEUTRAL:   return "NEUTRAL";
            default:                return "UNKNOWN";
        }
    }

    // Call once per H4 close. Engine aggregates D1 internally + updates EMA/slope
    // on D1 boundary.
    void on_h4_bar(double h4_high, double h4_low, double h4_close,
                    int64_t h4_close_ms) noexcept
    {
        const int64_t day_utc = h4_close_ms / 86400000LL;
        if (!d1_active_) {
            d1_active_ = true; d1_day_utc_ = day_utc;
            d1_open_ = h4_close; d1_high_ = h4_high;
            d1_low_  = h4_low;   d1_close_ = h4_close;
            return;
        }
        if (day_utc != d1_day_utc_) {
            const double bar_close = d1_close_;
            _on_d1_close(bar_close);
            d1_day_utc_ = day_utc;
            d1_open_ = h4_close; d1_high_ = h4_high;
            d1_low_  = h4_low;   d1_close_ = h4_close;
        } else {
            if (h4_high > d1_high_) d1_high_ = h4_high;
            if (h4_low  < d1_low_)  d1_low_  = h4_low;
            d1_close_ = h4_close;
        }
    }

    void _on_d1_close(double bar_close) noexcept {
        // Update EMA200
        const double a = 2.0 / (ema_period + 1);
        if (ema_count_ == 0) ema_ = bar_close;
        else ema_ = a * bar_close + (1 - a) * ema_;
        ++ema_count_;

        // Slope buffer
        ema_history_.push_back(ema_);
        while ((int)ema_history_.size() > slope_lookback + 1) ema_history_.pop_front();

        // Compute slope: (current_ema - ema_lookback_ago) / ema_lookback_ago
        if ((int)ema_history_.size() >= slope_lookback + 1 && ema_count_ >= ema_period) {
            const double cur = ema_history_.back();
            const double old = ema_history_.front();
            current_slope_ = (cur - old) / old;
            if (current_slope_ > slope_threshold)       current_regime_ = Regime::UPTREND;
            else if (current_slope_ < -slope_threshold) current_regime_ = Regime::DOWNTREND;
            else                                        current_regime_ = Regime::NEUTRAL;
        }
        ++bar_count_;
    }

    // Warm-seed from H4 CSV (CLAUDE.md mandate). Format: bar_start_ms,o,h,l,c
    // Returns count of bars seeded. Sets ema_count_ from history.
    size_t seed_from_h4_csv(const std::string& path) noexcept {
        const std::string actual = omega::resolve_seed_path(path);
        std::ifstream f(actual);
        if (!f.is_open()) {
            omega::seed_die("GoldD1TrendState", actual);  // [[noreturn]]
        }
        std::string line; std::getline(f, line);  // header
        size_t n = 0;
        while (std::getline(f, line)) {
            long long ts_ms_ll=0; double o=0,h=0,l=0,c=0;
            if (sscanf(line.c_str(), "%lld,%lf,%lf,%lf,%lf",
                       &ts_ms_ll, &o, &h, &l, &c) == 5) {
                on_h4_bar(h, l, c, static_cast<int64_t>(ts_ms_ll));
                ++n;
            }
        }
        if (n == 0) omega::seed_die("GoldD1TrendState", actual);  // [[noreturn]]
        printf("[SEED] GoldD1TrendState: %zu H4 bars -> EMA200=%.2f slope=%.4f regime=%s [%s]\n",
               n, ema_, current_slope_, regime_name(), actual.c_str());
        fflush(stdout);
        return n;
    }
};

// Singleton accessor -- resolves include-order issues. Engines call
// omega::gold_d1_trend() instead of referencing the global directly,
// so they don't need to be included after the global declaration.
inline GoldD1TrendState& gold_d1_trend() noexcept {
    static GoldD1TrendState inst;
    return inst;
}

} // namespace omega
