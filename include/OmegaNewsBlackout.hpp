#pragma once
// ==============================================================================
// OmegaNewsBlackout.hpp
// Economic news calendar integration — prevents trading during high-impact events.
//
// Hardcoded recurring events (weekly schedule):
//   - NFP (Non-Farm Payrolls): 1st Friday 13:30 UTC
//   - FOMC (Federal Reserve): typically 6 meetings/year, 19:00 UTC announcement
//   - CPI (US): 2nd Wednesday-ish 13:30 UTC — approximated as any Wed 13:25-13:40 UTC
//   - EIA Oil inventory: Wednesday 15:30 UTC
//   - BoE/ECB: various — approximated on configurable list
//
// Dynamic: also provides a manual blackout API so the operator can add
// ad-hoc blackouts (e.g. "block all trading for next 30 minutes") from config
// or in future from a live economic calendar HTTP feed.
//
// Usage:
//   #include "OmegaNewsBlackout.hpp"
//   static omega::news::NewsBlackout g_news_blackout;
//   g_news_blackout.configure(cfg);
//
//   if (g_news_blackout.is_blocked(symbol, now_sec)) { skip entry; }
// ==============================================================================

#include <string>
#include <vector>
#include <unordered_set>
#include <chrono>
#include <ctime>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <algorithm>

namespace omega { namespace news {

// ─────────────────────────────────────────────────────────────────────────────
// BlackoutWindow — a time window during which certain symbols are blocked
// ─────────────────────────────────────────────────────────────────────────────
struct BlackoutWindow {
    int64_t start_utc;   // unix seconds
    int64_t end_utc;     // unix seconds

    // empty set = ALL symbols blocked; otherwise only listed symbols
    std::unordered_set<std::string> symbols;

    std::string label;   // human-readable "NFP", "FOMC", etc.

    bool covers(int64_t ts, const std::string& sym) const noexcept {
        if (ts < start_utc || ts >= end_utc) return false;
        if (symbols.empty()) return true;
        return symbols.count(sym) > 0;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// RecurringEventScheduler
// Generates blackout windows for the current week based on UTC calendar.
// Does NOT require internet access — uses known weekly/monthly schedule.
// ─────────────────────────────────────────────────────────────────────────────
class RecurringEventScheduler {
public:
    // Blackout before/after high-impact events
    // "pre" = minutes before scheduled time to block entries
    // "post" = minutes after to allow market to settle
    int pre_minutes_nfp   = 5;
    int post_minutes_nfp  = 15;
    int pre_minutes_fomc  = 10;
    int post_minutes_fomc = 30;
    int pre_minutes_cpi   = 5;
    int post_minutes_cpi  = 10;
    int pre_minutes_eia   = 5;
    int post_minutes_eia  = 10;
    int pre_minutes_cb    = 10;  // central bank (BoE, ECB, RBA, BoJ)
    int post_minutes_cb   = 20;

    // Enabled flags
    bool block_nfp  = true;
    bool block_fomc = true;
    bool block_cpi  = true;
    bool block_eia  = true;   // only OIL/BRENT affected
    bool block_cb   = true;   // central bank decisions

    // Build blackout windows for the current week
    std::vector<BlackoutWindow> windows_for_week(int64_t any_ts_in_week) const {
        std::vector<BlackoutWindow> result;
        struct tm week_start = utc_monday(any_ts_in_week);

        // EIA Oil Inventory — every Wednesday 15:30 UTC
        if (block_eia) {
            const int64_t wed = epoch_of_weekday(week_start, 3, 15, 30); // Wed = day_offset 2
            if (wed > 0) {
                BlackoutWindow w;
                w.start_utc = wed - pre_minutes_eia  * 60;
                w.end_utc   = wed + post_minutes_eia * 60;
                w.symbols   = {"USOIL.F", "BRENT"};
                w.label     = "EIA_OIL";
                result.push_back(w);
            }
        }

        // NFP — 1st Friday of the month, 13:30 UTC
        // Approximate: we block all Fridays 13:25-13:50 UTC (false positives OK,
        // missing a Friday is safe, losing a few Friday London trades is worth it)
        if (block_nfp) {
            const int64_t fri = epoch_of_weekday(week_start, 5, 13, 30); // Fri = day_offset 4
            if (fri > 0) {
                BlackoutWindow w;
                w.start_utc = fri - pre_minutes_nfp  * 60;
                w.end_utc   = fri + post_minutes_nfp * 60;
                // NFP affects all USD pairs and US indices
                w.symbols   = {"US500.F","USTEC.F","DJ30.F","NAS100",
                                "EURUSD","GBPUSD","AUDUSD","NZDUSD","USDJPY",
                                "GOLD.F","XAGUSD","USOIL.F","BRENT"};
                w.label     = "NFP";
                result.push_back(w);
            }
        }

        // US CPI — approximated as 2nd Wednesday, 13:30 UTC
        // Conservative: block ALL Wednesdays 13:25 UTC as potential data release
        if (block_cpi) {
            const int64_t wed = epoch_of_weekday(week_start, 3, 13, 30);
            if (wed > 0) {
                BlackoutWindow w;
                w.start_utc = wed - pre_minutes_cpi  * 60;
                w.end_utc   = wed + post_minutes_cpi * 60;
                w.symbols   = {"US500.F","USTEC.F","DJ30.F","NAS100",
                                "EURUSD","GBPUSD","GOLD.F","XAGUSD"};
                w.label     = "CPI_APPROX";
                result.push_back(w);
            }
        }

        // FOMC — 8 meetings per year, Wednesdays 19:00 UTC announcement
        // We block ALL Wednesdays 18:55-19:30 UTC as conservative approximation
        // This is ~2 Wednesday sessions per month blocked — acceptable trade-off
        if (block_fomc) {
            const int64_t wed = epoch_of_weekday(week_start, 3, 19, 0);
            if (wed > 0) {
                BlackoutWindow w;
                w.start_utc = wed - pre_minutes_fomc  * 60;
                w.end_utc   = wed + post_minutes_fomc * 60;
                // FOMC affects all USD instruments + equities
                w.symbols   = {}; // ALL symbols
                w.label     = "FOMC_APPROX";
                result.push_back(w);
            }
        }

        // ECB decision — Thursdays, 12:15 UTC (approx 6x per year)
        // Conservative: block equity and EUR instruments Thursdays 12:10-12:40 UTC
        if (block_cb) {
            const int64_t thu = epoch_of_weekday(week_start, 4, 12, 15); // Thu = day_offset 3
            if (thu > 0) {
                BlackoutWindow w;
                w.start_utc = thu - pre_minutes_cb  * 60;
                w.end_utc   = thu + post_minutes_cb * 60;
                w.symbols   = {"EURUSD","GBPUSD","GER40","ESTX50","UK100"};
                w.label     = "ECB_APPROX";
                result.push_back(w);
            }
        }

        return result;
    }

private:
    // Returns UTC epoch for day-of-week (1=Mon…5=Fri) at HH:MM in the same week as week_start
    // week_start = struct tm of Monday 00:00 UTC for the current week
    static int64_t epoch_of_weekday(const struct tm& monday, int day_1indexed,
                                    int hour, int min) noexcept {
        // day_1indexed: 1=Mon, 2=Tue, 3=Wed, 4=Thu, 5=Fri
        struct tm t = monday;
        t.tm_mday  += (day_1indexed - 1);
        t.tm_hour   = hour;
        t.tm_min    = min;
        t.tm_sec    = 0;
#ifdef _WIN32
        const int64_t ep = static_cast<int64_t>(_mkgmtime(&t));
#else
        const int64_t ep = static_cast<int64_t>(timegm(&t));
#endif
        return (ep < 0) ? 0 : ep;
    }

    // Returns struct tm for Monday 00:00 UTC of the week containing ts
    static struct tm utc_monday(int64_t ts) noexcept {
        time_t t = static_cast<time_t>(ts);
        struct tm ti{};
#ifdef _WIN32
        gmtime_s(&ti, &t);
#else
        gmtime_r(&t, &ti);
#endif
        // tm_wday: 0=Sun, 1=Mon…6=Sat
        int days_since_monday = (ti.tm_wday == 0) ? 6 : (ti.tm_wday - 1);
        t -= static_cast<time_t>(days_since_monday) * 86400;
#ifdef _WIN32
        gmtime_s(&ti, &t);
#else
        gmtime_r(&t, &ti);
#endif
        ti.tm_hour = 0; ti.tm_min = 0; ti.tm_sec = 0;
        return ti;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// NewsBlackout — main class used by main.cpp
// ─────────────────────────────────────────────────────────────────────────────
class NewsBlackout {
public:
    RecurringEventScheduler scheduler;
    bool enabled = true;

    // Manual blackouts: operator can add/clear these from config or runtime
    std::vector<BlackoutWindow> manual_windows;

    // Cache for this week's generated windows (regenerated once per UTC day)
    mutable std::mutex          cache_mtx_;
    mutable std::vector<BlackoutWindow> cached_windows_;
    mutable int64_t             cache_day_ = -1;  // UTC day number of last build

    // ── Main check ────────────────────────────────────────────────────────────
    bool is_blocked(const std::string& symbol, int64_t now_sec) const {
        if (!enabled) return false;

        // Rebuild window cache if stale (daily rotation)
        rebuild_cache_if_needed(now_sec);

        for (const auto& w : cached_windows_) {
            if (w.covers(now_sec, symbol)) {
                // Throttle logging to once per 30s (avoid spam)
                static thread_local int64_t last_log = 0;
                if (now_sec - last_log > 30) {
                    last_log = now_sec;
                    std::printf("[NEWS-BLACKOUT] %s BLOCKED by %s (window %lld-%lld)\n",
                                symbol.c_str(), w.label.c_str(),
                                (long long)w.start_utc, (long long)w.end_utc);
                }
                return true;
            }
        }
        return false;
    }

    // Add a manual blackout (e.g. "block all for 30 minutes")
    void add_manual(int64_t start_sec, int64_t end_sec,
                    const std::string& label,
                    std::unordered_set<std::string> syms = {}) {
        std::lock_guard<std::mutex> lk(cache_mtx_);
        BlackoutWindow w;
        w.start_utc = start_sec;
        w.end_utc   = end_sec;
        w.label     = label;
        w.symbols   = std::move(syms);
        manual_windows.push_back(w);
        cache_day_ = -1;  // force rebuild
    }

    // Remove all expired manual blackouts
    void prune_manual(int64_t now_sec) {
        std::lock_guard<std::mutex> lk(cache_mtx_);
        manual_windows.erase(
            std::remove_if(manual_windows.begin(), manual_windows.end(),
                [now_sec](const BlackoutWindow& w){ return w.end_utc <= now_sec; }),
            manual_windows.end());
    }

    // Print today's blackout schedule to stdout (call once at startup)
    void print_schedule(int64_t now_sec) const {
        rebuild_cache_if_needed(now_sec);
        std::lock_guard<std::mutex> lk(cache_mtx_);
        std::printf("[NEWS-BLACKOUT] %zu windows scheduled for this week:\n",
                    cached_windows_.size());
        for (const auto& w : cached_windows_) {
            struct tm ts{}, te{};
            time_t s = (time_t)w.start_utc, e = (time_t)w.end_utc;
#ifdef _WIN32
            gmtime_s(&ts, &s); gmtime_s(&te, &e);
#else
            gmtime_r(&s, &ts); gmtime_r(&e, &te);
#endif
            std::printf("  [NEWS] %-16s  %02d:%02d-%02d:%02d UTC  syms=%s\n",
                        w.label.c_str(),
                        ts.tm_hour, ts.tm_min, te.tm_hour, te.tm_min,
                        w.symbols.empty() ? "ALL" : "(specific)");
        }
    }

private:
    void rebuild_cache_if_needed(int64_t now_sec) const {
        const int64_t day = now_sec / 86400;
        std::lock_guard<std::mutex> lk(cache_mtx_);
        if (day == cache_day_) return;
        cache_day_ = day;
        cached_windows_ = scheduler.windows_for_week(now_sec);
        // Append manual windows
        for (const auto& w : manual_windows)
            cached_windows_.push_back(w);
    }
};

}} // namespace omega::news
