// ==============================================================================
// OpenBbProxy.cpp
//
// libcurl-based HTTPS client for OpenBB Hub. See OpenBbProxy.hpp for the full
// design notes.
//
// This translation unit is gated on `#ifndef OMEGA_BACKTEST` so the backtester
// targets stay free of the libcurl dependency. The matching CMake side adds
// find_package(CURL REQUIRED) and links CURL::libcurl into the Omega target
// only.
//
// Endpoint base:
//   https://api.openbb.co/api/v1/<route>?<query>
//
// Authentication:
//   Authorization: Bearer <OMEGA_OPENBB_TOKEN>
//
// If OMEGA_OPENBB_MOCK=1 is set, the proxy bypasses the network entirely and
// returns canned synthetic JSON shaped like an OpenBB OBBject (so the route
// reshape code in OmegaApiServer.cpp does not branch on mock-vs-real).
//
// Windows include order:
//   <winsock2.h> + <ws2tcpip.h> MUST come BEFORE <curl/curl.h>. libcurl's
//   header references curl_socket_t / sockaddr / fd_set which are typedef'd
//   in winsock2.h on Windows, and including curl.h before winsock2.h gives
//   a cascade of MSVC C2061 / C2079 / C3646 errors. The repo-level CMake
//   defines _WINSOCKAPI_ globally to keep <windows.h> from pulling in the
//   older winsock.h, so we have a clean field for winsock2 here. Same idiom
//   appears in OmegaApiServer.cpp.
// ==============================================================================

#ifndef OMEGA_BACKTEST

#include "OpenBbProxy.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <curl/curl.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

namespace omega {

namespace {

constexpr const char* kBaseUrl = "https://api.openbb.co/api/v1/";

// Process-global curl_global_init guard. curl docs say it's safe to call
// curl_global_init repeatedly, but it is NOT thread-safe before the first
// call returns. We gate it behind a std::call_once.
std::once_flag g_curl_init_flag;

void ensure_curl_global_init()
{
    std::call_once(g_curl_init_flag, []() {
        const CURLcode rc = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (rc != CURLE_OK) {
            std::cerr << "[OpenBbProxy] curl_global_init failed: "
                      << curl_easy_strerror(rc) << "\n";
        }
    });
}

int64_t now_ms()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(
               steady_clock::now().time_since_epoch()).count();
}

size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    std::string* body = static_cast<std::string*>(userdata);
    const size_t bytes = size * nmemb;
    body->append(ptr, bytes);
    return bytes;
}

std::string env_or_empty(const char* name)
{
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

bool env_truthy(const char* name)
{
    const std::string v = env_or_empty(name);
    if (v.empty()) return false;
    // Accept "1", "true", "TRUE", "yes", "YES" as truthy. Anything else is
    // treated as off so a stray OMEGA_OPENBB_MOCK=0 does not enable mocks.
    if (v == "1" || v == "true" || v == "TRUE" || v == "yes" || v == "YES") {
        return true;
    }
    return false;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Singleton
// ─────────────────────────────────────────────────────────────────────────────

OpenBbProxy& OpenBbProxy::instance()
{
    static OpenBbProxy s_instance;
    return s_instance;
}

OpenBbProxy::OpenBbProxy()
    : token_(env_or_empty("OMEGA_OPENBB_TOKEN"))
    , mock_(env_truthy("OMEGA_OPENBB_MOCK"))
{
    if (mock_) {
        std::cout << "[OpenBbProxy] mock mode (OMEGA_OPENBB_MOCK=1) -- "
                  << "synthetic data, no network calls\n";
    } else if (token_.empty()) {
        std::cout << "[OpenBbProxy] OMEGA_OPENBB_TOKEN not set -- OpenBB "
                  << "routes will return HTTP 503 until the token is "
                  << "provided\n";
    } else {
        std::cout << "[OpenBbProxy] live mode -- token configured ("
                  << token_.size() << " chars), base " << kBaseUrl << "\n";
        ensure_curl_global_init();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Public get()
// ─────────────────────────────────────────────────────────────────────────────

OpenBbResult OpenBbProxy::get(const std::string& route,
                              const std::string& query,
                              int64_t            ttl_ms)
{
    // Build the cache key (full URL after the base) and the URL itself.
    std::string url = kBaseUrl;
    url += route;
    if (!query.empty()) {
        url += "?";
        url += query;
    }

    // ── Cache lookup ───────────────────────────────────────────────────────
    if (ttl_ms > 0) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = cache_.find(url);
        if (it != cache_.end()) {
            const int64_t age = now_ms() - it->second.stored_ms;
            if (age <= it->second.ttl_ms) {
                OpenBbResult r = it->second.result;
                r.from_cache = true;
                // Touch LRU.
                lru_.remove(url);
                lru_.push_front(url);
                return r;
            }
            // Stale -- evict and fall through to fetch.
            cache_.erase(it);
            lru_.remove(url);
        }
    }

    // ── Fetch ──────────────────────────────────────────────────────────────
    OpenBbResult r;
    if (mock_) {
        r = fetch_mock(route, query);
    } else if (token_.empty()) {
        r = OpenBbResult{
            503,
            R"({"error":"OPENBB_TOKEN_NOT_SET","detail":)"
            R"("Set the OMEGA_OPENBB_TOKEN environment variable on the )"
            R"(host running Omega.exe, or set OMEGA_OPENBB_MOCK=1 for )"
            R"(synthetic data."})",
            false,
        };
    } else {
        r = fetch_remote(url);
    }

    // ── Cache store (only for 2xx) ────────────────────────────────────────
    if (ttl_ms > 0 && r.status >= 200 && r.status < 300) {
        std::lock_guard<std::mutex> lk(mu_);
        cache_[url] = CacheEntry{r, now_ms(), ttl_ms};
        lru_.push_front(url);
        while (lru_.size() > kMaxCacheEntries) {
            const std::string victim = lru_.back();
            lru_.pop_back();
            cache_.erase(victim);
        }
    }

    return r;
}

// ─────────────────────────────────────────────────────────────────────────────
// fetch_remote -- real HTTPS via libcurl
// ─────────────────────────────────────────────────────────────────────────────

OpenBbResult OpenBbProxy::fetch_remote(const std::string& url)
{
    ensure_curl_global_init();

    std::string body;
    long        http_code = 0;

    CURL* curl = curl_easy_init();
    if (!curl) {
        return OpenBbResult{
            504,
            R"({"error":"curl_init_failed"})",
            false,
        };
    }

    curl_slist* headers = nullptr;
    {
        std::string auth = "Authorization: Bearer ";
        auth += token_;
        headers = curl_slist_append(headers, auth.c_str());
    }
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, "User-Agent: Omega-Terminal/1.0");

    curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL,       1L);

    const CURLcode rc = curl_easy_perform(curl);

    if (rc != CURLE_OK) {
        std::string err = R"({"error":"curl_perform_failed","detail":")";
        // The libcurl error string can contain quotes; we strip them roughly
        // so the JSON stays valid. This is best-effort; the UI just renders it.
        const char* cerr = curl_easy_strerror(rc);
        for (const char* p = cerr; *p; ++p) {
            if (*p == '"' || *p == '\\') err += '\'';
            else                          err += *p;
        }
        err += "\"}";
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return OpenBbResult{504, err, false};
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    return OpenBbResult{static_cast<int>(http_code), body, false};
}

// ─────────────────────────────────────────────────────────────────────────────
// fetch_mock -- synthetic JSON shaped like an OpenBB OBBject
//
// Returns a small but realistic-looking response per route so the UI can be
// developed and verified without an OpenBB account. Provider field is set to
// "mock" so a panel can surface "MOCK" in the corner if it wants.
//
// Routes covered (matches the four Step-5 routes in OmegaApiServer.cpp):
//   news/world                                     -> INTEL
//   fixedincome/government/treasury_rates          -> CURV
//   equity/price/quote                             -> WEI
//   equity/discovery/{active|gainers|losers}       -> MOV
// Anything else returns an empty results array with provider "mock".
//
// Raw-string delimiter note:
//   The standard `R"(...)"` form terminates at the first `)"` it sees, which
//   collides with literal text containing `)"` -- e.g. `"name":"X ETF (mock)"`.
//   Where that risk exists below we use a custom delimiter such as `R"X(...)X"`
//   so the raw string only terminates at the matching `)X"`. The mock JSON
//   strings without parens (news/world, treasury_rates, the outer wrapper of
//   discovery) keep the simpler `R"(...)"` form.
// ─────────────────────────────────────────────────────────────────────────────

OpenBbResult OpenBbProxy::fetch_mock(const std::string& route,
                                     const std::string& query)
{
    auto envelope = [](const std::string& results_json) {
        std::string out = "{\"results\":";
        out += results_json;
        out += ",\"provider\":\"mock\",\"warnings\":null,\"chart\":null,";
        out += "\"extra\":{\"mock\":true}}";
        return out;
    };

    if (route == "news/world") {
        const std::string results = R"([
  {"title":"Fed holds rates as inflation cools","text":"Synthetic Step-5 mock body. Replace with live OpenBB news once OMEGA_OPENBB_TOKEN is set.","date":"2026-05-01T13:30:00Z","url":"https://example.com/mock-1","source":"MOCK"},
  {"title":"Treasury 10Y yield slips below 4.20%","text":"Synthetic Step-5 mock body.","date":"2026-05-01T13:00:00Z","url":"https://example.com/mock-2","source":"MOCK"},
  {"title":"S&P 500 reclaims prior week high on broadening breadth","text":"Synthetic Step-5 mock body.","date":"2026-05-01T12:30:00Z","url":"https://example.com/mock-3","source":"MOCK"},
  {"title":"Crude oil holds $78 amid OPEC chatter","text":"Synthetic Step-5 mock body.","date":"2026-05-01T11:45:00Z","url":"https://example.com/mock-4","source":"MOCK"},
  {"title":"Gold steady near $2,310 as USD eases","text":"Synthetic Step-5 mock body.","date":"2026-05-01T11:00:00Z","url":"https://example.com/mock-5","source":"MOCK"}
])";
        return OpenBbResult{200, envelope(results), false};
    }

    if (route == "fixedincome/government/treasury_rates") {
        const std::string results = R"([
  {"date":"2026-05-01","maturity":"month_1","rate":4.95},
  {"date":"2026-05-01","maturity":"month_3","rate":4.88},
  {"date":"2026-05-01","maturity":"month_6","rate":4.72},
  {"date":"2026-05-01","maturity":"year_1","rate":4.55},
  {"date":"2026-05-01","maturity":"year_2","rate":4.30},
  {"date":"2026-05-01","maturity":"year_3","rate":4.18},
  {"date":"2026-05-01","maturity":"year_5","rate":4.10},
  {"date":"2026-05-01","maturity":"year_7","rate":4.15},
  {"date":"2026-05-01","maturity":"year_10","rate":4.20},
  {"date":"2026-05-01","maturity":"year_20","rate":4.45},
  {"date":"2026-05-01","maturity":"year_30","rate":4.40}
])";
        return OpenBbResult{200, envelope(results), false};
    }

    if (route == "equity/price/quote") {
        // Pick a result set that loosely matches the symbols the WEI panel
        // requested. We don't bother parsing the symbol list -- the UI will
        // accept whatever symbols come back and display them.
        std::string symbols = "SPY,QQQ,DIA,IWM";
        const std::string needle = "symbol=";
        const auto pos = query.find(needle);
        if (pos != std::string::npos) {
            const auto end = query.find('&', pos);
            symbols = query.substr(pos + needle.size(),
                                   (end == std::string::npos)
                                     ? std::string::npos
                                     : end - pos - needle.size());
        }
        std::string results = "[";
        size_t start = 0;
        bool first = true;
        while (start <= symbols.size()) {
            const auto comma = symbols.find(',', start);
            const std::string sym = symbols.substr(
                start,
                (comma == std::string::npos) ? std::string::npos : comma - start);
            if (!sym.empty()) {
                if (!first) results += ",";
                first = false;
                // Vary the mock prices slightly per symbol so the UI shows
                // motion when polled.
                int seed = 0;
                for (char c : sym) seed = (seed * 31 + (unsigned char)c) & 0xFFF;
                const double price  = 100.0 + (seed % 400);
                const double change = ((seed % 41) - 20) / 10.0;
                const double pct    = change / price * 100.0;
                char buf[256];
                // NOTE: custom raw-string delimiter `X(...)X` because the
                // literal text contains `(mock)"`, which would prematurely
                // terminate the default `R"(...)"` form at the first `)"`.
                std::snprintf(buf, sizeof(buf),
                    R"X({"symbol":"%s","name":"%s ETF (mock)","last_price":%.2f,)X"
                    R"X("change":%.2f,"change_percent":%.4f,"volume":%d,)X"
                    R"X("prev_close":%.2f})X",
                    sym.c_str(), sym.c_str(), price, change, pct,
                    1000000 + (seed * 137), price - change);
                results += buf;
            }
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        results += "]";
        return OpenBbResult{200, envelope(results), false};
    }

    if (route == "equity/discovery/active" ||
        route == "equity/discovery/gainers" ||
        route == "equity/discovery/losers") {
        const bool losers = (route == "equity/discovery/losers");
        const double sgn  = losers ? -1.0 : 1.0;
        char buf[2048];
        std::snprintf(buf, sizeof(buf), R"([
  {"symbol":"AAPL","name":"Apple Inc","price":182.50,"change":%.2f,"percent_change":%.4f,"volume":58000000},
  {"symbol":"NVDA","name":"NVIDIA Corp","price":910.25,"change":%.2f,"percent_change":%.4f,"volume":42000000},
  {"symbol":"MSFT","name":"Microsoft","price":410.10,"change":%.2f,"percent_change":%.4f,"volume":31000000},
  {"symbol":"TSLA","name":"Tesla Inc","price":188.60,"change":%.2f,"percent_change":%.4f,"volume":97000000},
  {"symbol":"AMD","name":"Advanced Micro Devices","price":162.40,"change":%.2f,"percent_change":%.4f,"volume":48000000},
  {"symbol":"META","name":"Meta Platforms","price":498.75,"change":%.2f,"percent_change":%.4f,"volume":22000000},
  {"symbol":"AMZN","name":"Amazon.com","price":182.30,"change":%.2f,"percent_change":%.4f,"volume":36000000},
  {"symbol":"GOOGL","name":"Alphabet Inc","price":171.20,"change":%.2f,"percent_change":%.4f,"volume":24000000}
])",
            sgn *  3.10, sgn * 1.7299,
            sgn * 18.50, sgn * 2.0763,
            sgn *  4.60, sgn * 1.1340,
            sgn * 11.20, sgn * 6.3373,
            sgn *  6.80, sgn * 4.3675,
            sgn *  7.50, sgn * 1.5267,
            sgn *  2.40, sgn * 1.3358,
            sgn *  1.95, sgn * 1.1502);
        return OpenBbResult{200, envelope(std::string(buf)), false};
    }

    return OpenBbResult{200, envelope("[]"), false};
}

} // namespace omega

#endif // OMEGA_BACKTEST
