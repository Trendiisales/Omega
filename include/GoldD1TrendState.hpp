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
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <map>
#include <string>
#include <vector>
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
        append_live_h4_(h4_close_ms, h4_high, h4_low, h4_close);   // self-fresh seed (no-op until recording_)
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

    // ---- persistence + self-fresh seed (2026-06-23 stale-seed fix; mirrors RegimeState) ----
    //   This gate (consulted by GoldEngineStack + XauTrendFollow 2h/D1/4h) was seeding from a
    //   60-day-stale H4 CSV and reset every restart -> EMA200-slope detached from reality ->
    //   long_allowed()/short_allowed() wrong -> gold engines traded the wrong direction. Now it
    //   persists state (60s) + self-records live H4 bars so it can never silently go stale.
    std::string dump_path_; bool recording_ = false;
    void set_live_dump(const std::string& p) noexcept { dump_path_ = p; }
    void start_recording() noexcept { recording_ = true; }
    void reset() noexcept {
        ema_ = 0.0; ema_count_ = 0; ema_history_.clear(); bar_count_ = 0;
        d1_active_ = false; current_regime_ = Regime::UNKNOWN; current_slope_ = 0.0;
    }
    bool save_state(const std::string& path) const noexcept {
        std::ofstream f(path, std::ios::trunc);
        if (!f.is_open()) return false;
        f << "saved_ts="  << (long long)std::time(nullptr) << "\n"
          << "ema="        << ema_        << "\n"
          << "ema_count="  << ema_count_  << "\n"
          << "bar_count="  << bar_count_  << "\n"
          << "regime="     << (int)current_regime_ << "\n"
          << "slope="      << current_slope_       << "\n"
          << "hist_n="     << ema_history_.size()  << "\n";
        for (double v : ema_history_) f << v << "\n";
        return true;
    }
    bool load_state(const std::string& path, int max_age_s = 48 * 3600) noexcept {
        std::ifstream f(path);
        if (!f.is_open()) return false;
        int64_t saved_ts = 0; double e = 0, slope = 0; int ec = 0, bc = 0, reg = 0;
        std::vector<double> hist; std::string line;
        auto kv = [](const char* k, const std::string& s, double& out) -> bool {
            const size_t L = std::strlen(k);
            if (s.size() > L && s.compare(0, L, k) == 0 && s[L] == '=') { out = std::atof(s.c_str() + L + 1); return true; }
            return false;
        };
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            double d;
            if      (line.compare(0, 9, "saved_ts=") == 0) saved_ts = (int64_t)std::atoll(line.c_str() + 9);
            else if (kv("ema_count", line, d)) ec = (int)d;
            else if (kv("ema", line, d))       e = d;
            else if (kv("bar_count", line, d)) bc = (int)d;
            else if (kv("regime", line, d))    reg = (int)d;
            else if (kv("slope", line, d))     slope = d;
            else if (line.compare(0, 7, "hist_n=") == 0) { /* implicit */ }
            else if (line[0] == '-' || line[0] == '.' || (line[0] >= '0' && line[0] <= '9')) hist.push_back(std::atof(line.c_str()));
        }
        const int64_t age = (int64_t)std::time(nullptr) - saved_ts;
        if (saved_ts <= 0 || age < 0 || age > max_age_s || ec < ema_period || e <= 0.0) {
            printf("[GoldD1Trend][LOAD] reject (age=%llds stale/invalid) -> will warm-seed\n", (long long)age);
            return false;
        }
        ema_ = e; ema_count_ = ec; bar_count_ = bc; current_slope_ = slope;
        current_regime_ = (Regime)reg;
        ema_history_.assign(hist.begin(), hist.end());
        while ((int)ema_history_.size() > slope_lookback + 1) ema_history_.pop_front();
        printf("[GoldD1Trend][LOAD] restored EMA200=%.2f slope=%.4f regime=%s age=%llds\n",
               ema_, current_slope_, regime_name(), (long long)age);
        fflush(stdout);
        return true;
    }
    void append_live_h4_(int64_t ts_ms, double h, double l, double c) const noexcept {
        if (dump_path_.empty() || !recording_ || c <= 0.0) return;
        std::ofstream f(dump_path_, std::ios::app);
        if (f.is_open()) f << ts_ms << ',' << c << ',' << h << ',' << l << ',' << c << "\n";  // o,h,l,c (o=c placeholder)
    }

    // ---- boot re-seed of the live dump (S-2026-07-20, operator-ordered) ----
    //   A deploy-restart that lands just after an H4 close eats that close's append ->
    //   the dump's mtime+content go stale for up to the next H4 close (weekend: 52h) ->
    //   feeds_selftest RED + staleness nag while nothing is actually broken. Fix: at boot,
    //   rewrite the dump as the ts-deduped union of its existing rows and the nightly-
    //   refreshed warmup CSV (OmegaSeedRefresh 23:30Z), so mtime AND content are boot-fresh;
    //   any remaining content gap is bounded by warmup freshness, and a genuinely-stalled
    //   H4 writer still trips the monitor because between boots only live appends move mtime.
    //   Consumers unaffected: XauTF4h append_fresh_h4 and StallCompanion gold_4h_bull_ both
    //   want ordered ts,o,h,l,c closes and already tolerate warmup/dump overlap. Warmup ts
    //   is bar_START_ms -> normalised +4h to the dump's close-ts convention so the same bar
    //   dedupes against its live-appended row (live/dump row wins on collision). NEVER
    //   truncates to empty: writes only when the merge produced rows.
    size_t regen_live_dump_from_csv(const std::string& warmup_path) const noexcept {
        if (dump_path_.empty()) return 0;
        std::map<int64_t, std::string> rows;   // close_ts_ms -> "o,h,l,c"
        auto parse = [](const std::string& line, long long& ts, double& o, double& h,
                        double& l, double& c) -> bool {
            if (std::sscanf(line.c_str(), "%lld,%lf,%lf,%lf,%lf", &ts, &o, &h, &l, &c) != 5)
                return false;
            if (ts <= 0 || c <= 0.0) return false;
            if (ts < 100000000000LL) ts *= 1000LL;   // seconds-vs-ms guard (seed_refresh)
            return true;
        };
        size_t n_warm = 0, n_live = 0;
        {
            std::ifstream f(omega::resolve_seed_path(warmup_path));
            std::string line;
            while (f.is_open() && std::getline(f, line)) {
                long long ts; double o, h, l, c;
                if (!parse(line, ts, o, h, l, c)) continue;   // skips header
                char buf[128];
                std::snprintf(buf, sizeof(buf), "%.5f,%.5f,%.5f,%.5f", o, h, l, c);
                rows[ts + 14400000LL] = buf;                  // bar_start -> close ts
                ++n_warm;
            }
        }
        {
            std::ifstream f(dump_path_);
            std::string line;
            while (f.is_open() && std::getline(f, line)) {
                long long ts; double o, h, l, c;
                if (!parse(line, ts, o, h, l, c)) continue;
                char buf[128];
                std::snprintf(buf, sizeof(buf), "%.5f,%.5f,%.5f,%.5f", o, h, l, c);
                rows[ts] = buf;                               // live row wins on collision
                ++n_live;
            }
        }
        if (rows.empty()) {
            printf("[GoldD1Trend][DUMP-REGEN] no rows from warmup or dump -- dump left untouched\n");
            fflush(stdout);
            return 0;
        }
        std::ofstream out(dump_path_, std::ios::trunc);
        if (!out.is_open()) {
            printf("[GoldD1Trend][DUMP-REGEN] cannot rewrite '%s' -- dump left as-is\n", dump_path_.c_str());
            fflush(stdout);
            return 0;
        }
        for (const auto& kv : rows) out << kv.first << ',' << kv.second << "\n";
        const int64_t last_ts_s = rows.rbegin()->first / 1000;
        const int64_t age_min = ((int64_t)std::time(nullptr) - last_ts_s) / 60;
        printf("[GoldD1Trend][DUMP-REGEN] warmup=%zu live=%zu -> %zu merged rows, last bar age=%lldmin (dump boot-fresh)\n",
               n_warm, n_live, rows.size(), (long long)age_min);
        fflush(stdout);
        return rows.size();
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
