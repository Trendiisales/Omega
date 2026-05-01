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
// Routes covered:
//   Step 5:
//     news/world                                     -> INTEL
//     fixedincome/government/treasury_rates          -> CURV
//     equity/price/quote                             -> WEI (also QR in Step 6)
//     equity/discovery/{active|gainers|losers}       -> MOV
//   Step 6:
//     derivatives/options/chains                     -> OMON
//     equity/fundamental/{income,balance,cash}       -> FA components
//     equity/fundamental/{key_metrics,multiples}     -> KEY components
//     equity/fundamental/dividends                   -> DVD
//     equity/estimates/{consensus,surprise}          -> EE components
//     news/company                                   -> NI
//     equity/price/historical                        -> GP / HP
//     equity/profile                                 -> DES
//     currency/price/quote                           -> FXC
//     crypto/price/quote                             -> CRYPTO
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

    // ---------------------------------------------------------------------
    // Step 6: BB function suite mocks
    // ---------------------------------------------------------------------

    if (route == "derivatives/options/chains") {
        // OMON: small synthetic chain. Two expiries x five strikes x call+put.
        std::string sym = "MOCK";
        const std::string needle = "symbol=";
        const auto pos = query.find(needle);
        if (pos != std::string::npos) {
            const auto end = query.find('&', pos);
            sym = query.substr(pos + needle.size(),
                               (end == std::string::npos)
                                 ? std::string::npos
                                 : end - pos - needle.size());
        }
        std::string results = "[";
        const char* expiries[2] = { "2026-06-20", "2026-09-19" };
        const double strikes[5] = { 90, 95, 100, 105, 110 };
        bool first = true;
        for (size_t e = 0; e < 2; ++e) {
            for (size_t s = 0; s < 5; ++s) {
                for (int t = 0; t < 2; ++t) {
                    if (!first) results += ",";
                    first = false;
                    const bool is_call = (t == 0);
                    const double k = strikes[s];
                    const double iv = 0.22 + 0.01 * s + 0.05 * e;
                    const double bid = is_call ? std::max(0.05, 100.0 - k + 1.5) : std::max(0.05, k - 100.0 + 1.5);
                    const double ask = bid + 0.10;
                    const double mid = (bid + ask) / 2.0;
                    const double oi  = 1500 + 350 * (5 - (int)s);
                    const double vol = 250 + 80 * (int)s;
                    const double delta = is_call ? std::max(0.05, 1.0 - 0.1 * (k - 95)) : std::min(-0.05, -1.0 + 0.1 * (k - 95));
                    char buf[512];
                    std::snprintf(buf, sizeof(buf),
                        R"X({"underlying_symbol":"%s","contract_symbol":"%s%s%s%05d","expiration":"%s",)X"
                        R"X("strike":%.2f,"option_type":"%s","bid":%.2f,"ask":%.2f,"last_trade_price":%.2f,)X"
                        R"X("implied_volatility":%.4f,"open_interest":%.0f,"volume":%.0f,"delta":%.4f})X",
                        sym.c_str(), sym.c_str(), expiries[e], is_call ? "C" : "P", (int)(k * 1000),
                        expiries[e], k, is_call ? "call" : "put", bid, ask, mid, iv, oi, vol, delta);
                    results += buf;
                }
            }
        }
        results += "]";
        return OpenBbResult{200, envelope(results), false};
    }

    if (route == "equity/fundamental/income") {
        const std::string results = R"X([
  {"period_ending":"2025-12-31","fiscal_period":"FY2025","revenue":390000000000,"cost_of_revenue":215000000000,"gross_profit":175000000000,"operating_income":118000000000,"ebitda":133000000000,"net_income":98000000000,"eps_basic":6.40,"eps_diluted":6.36},
  {"period_ending":"2024-12-31","fiscal_period":"FY2024","revenue":383000000000,"cost_of_revenue":214000000000,"gross_profit":169000000000,"operating_income":114000000000,"ebitda":129000000000,"net_income":97000000000,"eps_basic":6.10,"eps_diluted":6.05},
  {"period_ending":"2023-12-31","fiscal_period":"FY2023","revenue":366000000000,"cost_of_revenue":213000000000,"gross_profit":153000000000,"operating_income":108000000000,"ebitda":123000000000,"net_income":94000000000,"eps_basic":5.90,"eps_diluted":5.85}
])X";
        return OpenBbResult{200, envelope(results), false};
    }

    if (route == "equity/fundamental/balance") {
        const std::string results = R"X([
  {"period_ending":"2025-12-31","fiscal_period":"FY2025","total_assets":350000000000,"total_current_assets":140000000000,"cash_and_short_term_investments":62000000000,"total_liabilities":260000000000,"total_current_liabilities":150000000000,"long_term_debt":95000000000,"short_term_debt":12000000000,"total_equity":90000000000},
  {"period_ending":"2024-12-31","fiscal_period":"FY2024","total_assets":340000000000,"total_current_assets":135000000000,"cash_and_short_term_investments":61000000000,"total_liabilities":255000000000,"total_current_liabilities":148000000000,"long_term_debt":97000000000,"short_term_debt":11000000000,"total_equity":85000000000},
  {"period_ending":"2023-12-31","fiscal_period":"FY2023","total_assets":332000000000,"total_current_assets":131000000000,"cash_and_short_term_investments":60000000000,"total_liabilities":250000000000,"total_current_liabilities":146000000000,"long_term_debt":99000000000,"short_term_debt":10000000000,"total_equity":82000000000}
])X";
        return OpenBbResult{200, envelope(results), false};
    }

    if (route == "equity/fundamental/cash") {
        const std::string results = R"X([
  {"period_ending":"2025-12-31","fiscal_period":"FY2025","cash_from_operating_activities":120000000000,"cash_from_investing_activities":-25000000000,"cash_from_financing_activities":-95000000000,"capital_expenditure":-12000000000,"free_cash_flow":108000000000},
  {"period_ending":"2024-12-31","fiscal_period":"FY2024","cash_from_operating_activities":115000000000,"cash_from_investing_activities":-22000000000,"cash_from_financing_activities":-92000000000,"capital_expenditure":-11000000000,"free_cash_flow":104000000000},
  {"period_ending":"2023-12-31","fiscal_period":"FY2023","cash_from_operating_activities":110000000000,"cash_from_investing_activities":-21000000000,"cash_from_financing_activities":-89000000000,"capital_expenditure":-11000000000,"free_cash_flow":99000000000}
])X";
        return OpenBbResult{200, envelope(results), false};
    }

    if (route == "equity/fundamental/key_metrics") {
        const std::string results = R"X([
  {"market_cap":2900000000000,"enterprise_value":2950000000000,"pe_ratio":29.5,"forward_pe":27.1,"peg_ratio":2.0,"price_to_book":42.0,"price_to_sales":7.4,"ev_to_sales":7.6,"ev_to_ebitda":22.2,"dividend_yield":0.0050,"payout_ratio":0.16,"beta":1.20,"return_on_equity":1.55,"return_on_assets":0.28,"debt_to_equity":1.18,"current_ratio":0.92,"quick_ratio":0.88,"profit_margin":0.25,"operating_margin":0.30}
])X";
        return OpenBbResult{200, envelope(results), false};
    }

    if (route == "equity/fundamental/multiples") {
        const std::string results = R"X([
  {"pe_ratio_ttm":29.5,"ev_to_ebitda_ttm":22.2,"price_to_sales_ttm":7.4,"price_to_book_quarterly":42.0,"earnings_yield_ttm":0.034,"free_cash_flow_yield_ttm":0.037}
])X";
        return OpenBbResult{200, envelope(results), false};
    }

    if (route == "equity/fundamental/dividends") {
        const std::string results = R"X([
  {"ex_dividend_date":"2026-04-25","amount":0.25,"record_date":"2026-04-26","payment_date":"2026-05-15","declaration_date":"2026-04-10"},
  {"ex_dividend_date":"2026-01-25","amount":0.24,"record_date":"2026-01-26","payment_date":"2026-02-15","declaration_date":"2026-01-10"},
  {"ex_dividend_date":"2025-10-25","amount":0.24,"record_date":"2025-10-26","payment_date":"2025-11-15","declaration_date":"2025-10-10"},
  {"ex_dividend_date":"2025-07-25","amount":0.23,"record_date":"2025-07-26","payment_date":"2025-08-15","declaration_date":"2025-07-10"},
  {"ex_dividend_date":"2025-04-25","amount":0.23,"record_date":"2025-04-26","payment_date":"2025-05-15","declaration_date":"2025-04-10"}
])X";
        return OpenBbResult{200, envelope(results), false};
    }

    if (route == "equity/estimates/consensus") {
        const std::string results = R"X([
  {"symbol":"MOCK","fiscal_period":"Q2","fiscal_year":2026,"eps_avg":1.65,"eps_high":1.78,"eps_low":1.52,"revenue_avg":98000000000,"revenue_high":102000000000,"revenue_low":94000000000,"number_of_analysts":36}
])X";
        return OpenBbResult{200, envelope(results), false};
    }

    if (route == "equity/estimates/surprise") {
        const std::string results = R"X([
  {"symbol":"MOCK","date":"2026-04-30","fiscal_period":"Q1","fiscal_year":2026,"eps_actual":1.71,"eps_estimate":1.62,"eps_surprise":0.09,"surprise_percent":5.55},
  {"symbol":"MOCK","date":"2026-01-31","fiscal_period":"Q4","fiscal_year":2025,"eps_actual":2.18,"eps_estimate":2.10,"eps_surprise":0.08,"surprise_percent":3.81},
  {"symbol":"MOCK","date":"2025-10-30","fiscal_period":"Q3","fiscal_year":2025,"eps_actual":1.49,"eps_estimate":1.55,"eps_surprise":-0.06,"surprise_percent":-3.87},
  {"symbol":"MOCK","date":"2025-07-31","fiscal_period":"Q2","fiscal_year":2025,"eps_actual":1.40,"eps_estimate":1.36,"eps_surprise":0.04,"surprise_percent":2.94},
  {"symbol":"MOCK","date":"2025-04-30","fiscal_period":"Q1","fiscal_year":2025,"eps_actual":1.53,"eps_estimate":1.50,"eps_surprise":0.03,"surprise_percent":2.00}
])X";
        return OpenBbResult{200, envelope(results), false};
    }

    if (route == "news/company") {
        const std::string results = R"X([
  {"title":"MOCK Corp announces Q2 product launch","text":"Synthetic Step-6 mock body for the NI panel.","date":"2026-05-01T13:30:00Z","url":"https://example.com/ni-1","source":"MOCK","symbols":["MOCK"]},
  {"title":"Analyst upgrade lifts MOCK to $200 PT","text":"Synthetic Step-6 mock body.","date":"2026-05-01T12:00:00Z","url":"https://example.com/ni-2","source":"MOCK","symbols":["MOCK"]},
  {"title":"MOCK CFO sells 10k shares in pre-arranged plan","text":"Synthetic Step-6 mock body.","date":"2026-05-01T11:00:00Z","url":"https://example.com/ni-3","source":"MOCK","symbols":["MOCK"]},
  {"title":"Insider buying continues across MOCK suppliers","text":"Synthetic Step-6 mock body.","date":"2026-04-30T19:00:00Z","url":"https://example.com/ni-4","source":"MOCK","symbols":["MOCK"]},
  {"title":"MOCK and partner ink multi-year cloud deal","text":"Synthetic Step-6 mock body.","date":"2026-04-30T16:00:00Z","url":"https://example.com/ni-5","source":"MOCK","symbols":["MOCK"]}
])X";
        return OpenBbResult{200, envelope(results), false};
    }

    if (route == "equity/price/historical") {
        // Synthetic 60-bar daily series with a gentle uptrend + noise.
        std::string results = "[";
        const int N = 60;
        double price = 150.0;
        bool first = true;
        for (int i = N - 1; i >= 0; --i) {
            // Walk-back: bar i = N-1 is the most recent; bar 0 is the oldest.
            // To produce ascending-time output we generate from oldest forward.
        }
        for (int i = 0; i < N; ++i) {
            if (!first) results += ",";
            first = false;
            // Days back from today: (N-1-i).
            const int days_back = N - 1 - i;
            char date[16];
            // Use a simple synthetic date stamp; UI parses Date.parse() so the
            // exact day-of-week is irrelevant for chart rendering.
            std::snprintf(date, sizeof(date), "2026-%02d-%02d",
                          ((days_back / 30) ? 3 : 4),
                          1 + (days_back % 28));
            const double bar_drift = (i - N / 2) * 0.15;
            const double noise     = ((i * 31) % 17) / 10.0 - 0.85;
            const double close = price + bar_drift + noise;
            const double open  = close - 0.30;
            const double high  = std::max(open, close) + 0.85;
            const double low   = std::min(open, close) - 0.85;
            const double vol   = 1500000 + ((i * 137) % 700000);
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                R"X({"date":"%s","open":%.2f,"high":%.2f,"low":%.2f,"close":%.2f,"volume":%.0f,"adj_close":%.2f})X",
                date, open, high, low, close, vol, close);
            results += buf;
        }
        results += "]";
        return OpenBbResult{200, envelope(results), false};
    }

    if (route == "equity/profile") {
        std::string sym = "MOCK";
        const std::string needle = "symbol=";
        const auto pos = query.find(needle);
        if (pos != std::string::npos) {
            const auto end = query.find('&', pos);
            sym = query.substr(pos + needle.size(),
                               (end == std::string::npos)
                                 ? std::string::npos
                                 : end - pos - needle.size());
        }
        char buf[1024];
        std::snprintf(buf, sizeof(buf),
            R"X([{"symbol":"%s","name":"%s Corp (mock)","description":"Synthetic Step-6 mock company used by the DES panel for local development without an OpenBB account. Replace with live data by setting OMEGA_OPENBB_TOKEN on the host.","industry":"Software - Infrastructure","sector":"Technology","ipo_date":"2010-01-15","ceo":"Jane Doe","hq_country":"United States","hq_state":"California","hq_city":"Cupertino","employees":154000,"website":"https://example.com","exchange":"NASDAQ","currency":"USD","market_cap":2900000000000}])X",
            sym.c_str(), sym.c_str());
        return OpenBbResult{200, envelope(std::string(buf)), false};
    }

    if (route == "currency/price/quote") {
        std::string symbols = "EURUSD,GBPUSD,USDJPY";
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
                int seed = 0;
                for (char c : sym) seed = (seed * 31 + (unsigned char)c) & 0xFFF;
                const double price  = 0.5 + (seed % 200) / 100.0;
                const double change = ((seed % 21) - 10) / 10000.0;
                const double pct    = change / price * 100.0;
                char buf[320];
                std::snprintf(buf, sizeof(buf),
                    R"X({"symbol":"%s","name":"%s (mock)","last_price":%.5f,"bid":%.5f,"ask":%.5f,)X"
                    R"X("change":%.5f,"change_percent":%.4f,"volume":%d})X",
                    sym.c_str(), sym.c_str(), price, price - 0.00005, price + 0.00005,
                    change, pct, 250000 + (seed * 11));
                results += buf;
            }
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        results += "]";
        return OpenBbResult{200, envelope(results), false};
    }

    if (route == "crypto/price/quote") {
        std::string symbols = "BTC-USD,ETH-USD,SOL-USD";
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
                int seed = 0;
                for (char c : sym) seed = (seed * 31 + (unsigned char)c) & 0xFFF;
                const double price  = 100.0 + (seed * 23) % 70000;
                const double change = ((seed % 41) - 20) * 1.5;
                const double pct    = change / price * 100.0;
                const double mcap   = price * (10000000 + (seed * 71));
                char buf[320];
                std::snprintf(buf, sizeof(buf),
                    R"X({"symbol":"%s","name":"%s (mock)","last_price":%.2f,)X"
                    R"X("change":%.2f,"change_percent":%.4f,"volume":%d,"market_cap":%.0f})X",
                    sym.c_str(), sym.c_str(), price, change, pct,
                    1000000 + (seed * 211), mcap);
                results += buf;
            }
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        results += "]";
        return OpenBbResult{200, envelope(results), false};
    }

    return OpenBbResult{200, envelope("[]"), false};
}

} // namespace omega

#endif // OMEGA_BACKTEST
