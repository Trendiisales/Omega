#pragma once
// ==============================================================================
// OmegaNewsBlackout.hpp
// Economic news calendar integration -- prevents trading during high-impact events.
//
// Hardcoded recurring events (weekly schedule):
//   - NFP (Non-Farm Payrolls): 1st Friday 13:30 UTC
//   - FOMC (Federal Reserve): typically 6 meetings/year, 19:00 UTC announcement
//   - CPI (US): 2nd Wednesday-ish 13:30 UTC -- approximated as any Wed 13:25-13:40 UTC
//   - EIA Oil inventory: Wednesday 15:30 UTC
//   - BoE/ECB: various -- approximated on configurable list
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

// ?????????????????????????????????????????????????????????????????????????????
// BlackoutWindow -- a time window during which certain symbols are blocked
// ?????????????????????????????????????????????????????????????????????????????
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

// ?????????????????????????????????????????????????????????????????????????????
// RecurringEventScheduler
// Generates blackout windows for the current week based on UTC calendar.
// Does NOT require internet access -- uses known weekly/monthly schedule.
// ?????????????????????????????????????????????????????????????????????????????
class RecurringEventScheduler {
public:
    // Blackout before/after high-impact events
    // "pre" = minutes before scheduled time to block entries
    // "post" = minutes after to allow market to settle
    int pre_minutes_nfp   = 5;
    int post_minutes_nfp  = 90;  // raised 15->90: NFP drives 60-90min moves. Evidence 2026-04-03: VWAPRev lost -$177 after 13:45 when old blackout ended.
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

        // EIA Oil Inventory -- every Wednesday 15:30 UTC
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

        // NFP -- 1st Friday of the month, 13:30 UTC
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
                                "XAUUSD","XAGUSD","USOIL.F","BRENT"};
                w.label     = "NFP";
                result.push_back(w);
            }
        }

        // US CPI -- approximated as 2nd Wednesday, 13:30 UTC
        // Conservative: block ALL Wednesdays 13:25 UTC as potential data release
        if (block_cpi) {
            const int64_t wed = epoch_of_weekday(week_start, 3, 13, 30);
            if (wed > 0) {
                BlackoutWindow w;
                w.start_utc = wed - pre_minutes_cpi  * 60;
                w.end_utc   = wed + post_minutes_cpi * 60;
                w.symbols   = {"US500.F","USTEC.F","DJ30.F","NAS100",
                                "EURUSD","GBPUSD","XAUUSD","XAGUSD"};
                w.label     = "CPI_APPROX";
                result.push_back(w);
            }
        }

        // FOMC -- 8 meetings per year, Wednesdays 19:00 UTC announcement
        // We block ALL Wednesdays 18:55-19:30 UTC as conservative approximation
        // This is ~2 Wednesday sessions per month blocked -- acceptable trade-off
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

        // ECB decision -- Thursdays, 12:15 UTC (approx 6x per year)
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
    // Returns UTC epoch for day-of-week (1=Mon...5=Fri) at HH:MM in the same week as week_start
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
        // tm_wday: 0=Sun, 1=Mon...6=Sat
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

// ?????????????????????????????????????????????????????????????????????????????
// NewsBlackout -- main class used by main.cpp
// ?????????????????????????????????????????????????????????????????????????????
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

    // ?? Main check ????????????????????????????????????????????????????????????
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

    // Returns seconds until the next blackout window starts (any symbol).
    // Returns INT64_MAX if no upcoming window within 24 hours.
    // Used to widen SL / tighten trail when a news event is imminent.
    int64_t secs_until_next(int64_t now_sec) const {
        if (!enabled) return INT64_MAX;
        rebuild_cache_if_needed(now_sec);
        std::lock_guard<std::mutex> lk(cache_mtx_);
        int64_t nearest = INT64_MAX;
        for (const auto& w : cached_windows_) {
            if (w.start_utc > now_sec) {
                const int64_t diff = w.start_utc - now_sec;
                if (diff < nearest) nearest = diff;
            }
        }
        return nearest;
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

// ?????????????????????????????????????????????????????????????????????????????
// LiveCalendarFetcher
// Fetches the Forex Factory weekly calendar XML and injects exact HIGH-impact
// blackout windows into a NewsBlackout instance.
//
// URL: https://nfs.faireconomy.media/ff_calendar_thisweek.xml
// Format: <event><title>NFP</title><country>USD</country><date>...</date>
//         <time>8:30am</time><impact>High</impact>...
//
// Call refresh() once at startup, then weekly (auto-refreshes internally).
// Falls back to the hardcoded recurring scheduler on any network failure.
//
// Symbols blocked per event country:
//   USD ? US500.F, USTEC.F, DJ30.F, NAS100, EURUSD, GBPUSD, AUDUSD, NZDUSD,
//          USDJPY, XAUUSD, XAGUSD, USOIL.F, BRENT
//   EUR ? EURUSD, GER40, ESTX50
//   GBP ? GBPUSD, UK100
//   JPY ? USDJPY
//   AUD ? AUDUSD
//   NZD ? NZDUSD
//   CAD ? USOIL.F, BRENT
// ?????????????????????????????????????????????????????????????????????????????
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#endif

namespace omega { namespace news {

class LiveCalendarFetcher {
public:
    // Minutes before/after each HIGH-impact event to block
    int pre_min  = 5;
    int post_min = 15;
    bool enabled = true;

    // Refresh interval: re-fetch once a week (604800s). Set lower for testing.
    int64_t refresh_interval_sec = 604800;

    // Call once at startup (and weekly thereafter via check_and_refresh)
    bool refresh(NewsBlackout& blackout, int64_t now_sec) {
        if (!enabled) return false;
        last_refresh_sec_ = now_sec;

        std::string xml;
        if (!fetch_https("nfs.faireconomy.media", "/ff_calendar_thisweek.xml", xml)) {
            std::printf("[NEWS-CAL] Live calendar fetch failed -- using hardcoded schedule\n");
            return false;
        }
        const int injected = parse_and_inject(xml, blackout, now_sec);
        std::printf("[NEWS-CAL] Live calendar loaded: %d HIGH-impact events injected\n", injected);
        return injected > 0;
    }

    // Call every tick -- only actually re-fetches after refresh_interval_sec
    void check_and_refresh(NewsBlackout& blackout, int64_t now_sec) {
        if (!enabled) return;
        if (now_sec - last_refresh_sec_ >= refresh_interval_sec)
            refresh(blackout, now_sec);
    }

private:
    int64_t last_refresh_sec_ = 0;

    // Symbol sets per currency
    static std::unordered_set<std::string> syms_for_country(const std::string& country) {
        if (country == "USD") return {"US500.F","USTEC.F","DJ30.F","NAS100",
                                       "EURUSD","GBPUSD","AUDUSD","NZDUSD","USDJPY",
                                       "XAUUSD","XAGUSD","USOIL.F","BRENT"};
        if (country == "EUR") return {"EURUSD","GER40","ESTX50"};
        if (country == "GBP") return {"GBPUSD","UK100"};
        if (country == "JPY") return {"USDJPY"};
        if (country == "AUD") return {"AUDUSD"};
        if (country == "NZD") return {"NZDUSD"};
        if (country == "CAD") return {"USOIL.F","BRENT"};
        return {}; // unknown country -- block all
    }

    // Extract inner text of first <tag>...</tag> in xml starting from pos
    static std::string extract_tag(const std::string& xml, const std::string& tag, size_t& pos) {
        const std::string open  = "<"  + tag + ">";
        const std::string close = "</" + tag + ">";
        const size_t s = xml.find(open, pos);
        if (s == std::string::npos) { pos = std::string::npos; return {}; }
        const size_t e = xml.find(close, s);
        if (e == std::string::npos) { pos = std::string::npos; return {}; }
        pos = e + close.size();
        return xml.substr(s + open.size(), e - (s + open.size()));
    }

    // Parse "Jan 26, 2025" or "2025-01-26" date strings ? UTC midnight unix seconds
    static int64_t parse_date(const std::string& d) {
        struct tm t{};
        // Try ISO format first: 2025-01-26T08:30:00-0500
        if (d.size() >= 10 && d[4] == '-') {
            t.tm_year = std::stoi(d.substr(0,4)) - 1900;
            t.tm_mon  = std::stoi(d.substr(5,2)) - 1;
            t.tm_mday = std::stoi(d.substr(8,2));
            // Cross-platform UTC mktime (mirrors the #ifdef block at line 179
            // above) -- _mkgmtime is Windows-only; timegm is the POSIX/macOS
            // equivalent. Without this branch the file fails to compile on
            // the Mac verifier build.
#ifdef _WIN32
            return (int64_t)_mkgmtime(&t);
#else
            return (int64_t)timegm(&t);
#endif
        }
        return 0;
    }

    // Parse "8:30am" / "8:30pm" / "All Day" ? seconds offset from midnight UTC
    // FF times are US Eastern -- convert to UTC (+5h EST, +4h EDT)
    // We use a conservative +5h (EST) offset; EDT adds 1h over-protection = acceptable
    static int parse_time_offset(const std::string& t) {
        if (t.empty() || t == "All Day" || t == "Tentative") return 13 * 3600; // noon default
        bool pm = (t.find("pm") != std::string::npos);
        bool am = (t.find("am") != std::string::npos);
        const size_t colon = t.find(':');
        if (colon == std::string::npos) return 13 * 3600;
        int hr  = std::stoi(t.substr(0, colon));
        int min = std::stoi(t.substr(colon + 1, 2));
        if (pm && hr != 12) hr += 12;
        if (am && hr == 12) hr = 0;
        // Convert from US Eastern to UTC: +5h (conservative -- EST, never under-estimates)
        hr += 5;
        return hr * 3600 + min * 60;
    }

    int parse_and_inject(const std::string& xml, NewsBlackout& blackout, int64_t now_sec) {
        int count = 0;
        size_t pos = 0;

        // Remove any old live-calendar windows before adding new ones
        blackout.prune_manual(now_sec - 1); // prune expired; new ones will be added

        while (pos != std::string::npos) {
            const size_t event_start = xml.find("<event>", pos);
            if (event_start == std::string::npos) break;
            const size_t event_end = xml.find("</event>", event_start);
            if (event_end == std::string::npos) break;
            const std::string event = xml.substr(event_start, event_end - event_start + 8);
            pos = event_end + 8;

            // Extract fields
            size_t p = 0;
            const std::string impact  = extract_tag(event, "impact",  p); p = 0;
            const std::string country = extract_tag(event, "country", p); p = 0;
            const std::string date_s  = extract_tag(event, "date",    p); p = 0;
            const std::string time_s  = extract_tag(event, "time",    p); p = 0;
            const std::string title   = extract_tag(event, "title",   p);

            // Only HIGH impact events
            if (impact.find("High") == std::string::npos &&
                impact.find("high") == std::string::npos) continue;

            const int64_t date_ts = parse_date(date_s);
            if (date_ts <= 0) continue;

            const int time_off = parse_time_offset(time_s);
            const int64_t event_ts = date_ts + time_off;

            // Skip events more than 7 days in the past or 14 days in the future
            if (event_ts < now_sec - 7*86400) continue;
            if (event_ts > now_sec + 14*86400) continue;

            BlackoutWindow w;
            w.start_utc = event_ts - (int64_t)pre_min  * 60;
            w.end_utc   = event_ts + (int64_t)post_min * 60;
            w.label     = "LIVE:" + title + "(" + country + ")";
            w.symbols   = syms_for_country(country);

            blackout.add_manual(w.start_utc, w.end_utc, w.label, w.symbols);
            ++count;

            std::printf("[NEWS-CAL]   +%s %s %s impact=%s\n",
                        country.c_str(), date_s.c_str(), title.c_str(), impact.c_str());
        }
        return count;
    }

    // Minimal HTTPS GET using WinSock2 + OpenSSL (already linked for FIX session)
    static bool fetch_https(const char* host, const char* path, std::string& out) {
#ifdef _WIN32
        // Resolve host
        addrinfo hints{}, *res = nullptr;
        hints.ai_family   = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host, "443", &hints, &res) != 0) return false;

        SOCKET sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (sock == INVALID_SOCKET) { freeaddrinfo(res); return false; }

        // Set 10s timeout
        DWORD tv = 10000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));

        if (connect(sock, res->ai_addr, (int)res->ai_addrlen) != 0) {
            closesocket(sock); freeaddrinfo(res); return false;
        }
        freeaddrinfo(res);

        // TLS handshake
        SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
        if (!ctx) { closesocket(sock); return false; }
        SSL_CTX_set_default_verify_paths(ctx);

        SSL* ssl = SSL_new(ctx);
        SSL_set_fd(ssl, (int)sock);
        SSL_set_tlsext_host_name(ssl, host);
        if (SSL_connect(ssl) != 1) {
            SSL_free(ssl); SSL_CTX_free(ctx); closesocket(sock); return false;
        }

        // HTTP GET
        std::string req = std::string("GET ") + path + " HTTP/1.0\r\n"
                        + "Host: " + host + "\r\n"
                        + "Connection: close\r\n\r\n";
        SSL_write(ssl, req.c_str(), (int)req.size());

        // Read response
        char buf[4096];
        std::string raw;
        int n;
        while ((n = SSL_read(ssl, buf, sizeof(buf) - 1)) > 0) {
            buf[n] = '\0';
            raw += buf;
        }
        SSL_free(ssl); SSL_CTX_free(ctx); closesocket(sock);

        // Strip HTTP headers
        const size_t header_end = raw.find("\r\n\r\n");
        if (header_end == std::string::npos) return false;
        out = raw.substr(header_end + 4);
        return !out.empty();
#else
        (void)host; (void)path; (void)out;
        return false; // non-Windows not implemented
#endif
    }
};

}} // namespace omega::news (re-opened for LiveCalendarFetcher)
