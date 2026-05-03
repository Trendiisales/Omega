#pragma once
// ==============================================================================
// MarketDataProxy -- libcurl-based HTTPS client for free market-data providers.
//
// Step 7 of the Omega Terminal build (replaces OpenBbProxy from Step 5/6).
//
// Backs the same 17 Omega-Terminal routes as Step 5 + Step 6:
//
//   /api/v1/omega/intel  /omega/curv   /omega/wei    /omega/mov
//   /omega/omon          /omega/fa     /omega/key    /omega/dvd
//   /omega/ee            /omega/ni     /omega/gp     /omega/qr
//   /omega/hp            /omega/des    /omega/fxc    /omega/crypto
//   /omega/watch
//
// The Step-5/6 implementation pointed at https://api.openbb.co/api/v1/<route>
// which returns HTTP 404 -- OpenBB does not host a public REST endpoint; the
// OpenBB Platform is a Python sidecar library. Step 7 rips OpenBB out and
// dispatches each route to a free upstream:
//
//   Yahoo Finance (16 routes, no API key)
//     - query1.finance.yahoo.com/v7/finance/quote
//     - query1.finance.yahoo.com/v8/finance/chart/<sym>
//     - query2.finance.yahoo.com/v7/finance/options/<sym>
//     - query1.finance.yahoo.com/v10/finance/quoteSummary/<sym>
//     - query1.finance.yahoo.com/v1/finance/search
//
//   FRED (1 route -- CURV; requires OMEGA_FRED_KEY at process start)
//     - api.stlouisfed.org/fred/series/observations
//
// Public API is byte-compatible with the retired OpenBbProxy:
//
//   MarketDataProxy::instance().get(route, query, ttl_ms)
//     -> MarketDataResult { status, body, from_cache }
//
// where `route` is the Step-5/6 OpenBB-style route name (e.g.
// "equity/price/quote") and `body` is JSON in the same OpenBB-OBBject-shaped
// envelope the panels already parse:
//
//   { "results": [...], "provider": "yahoo"|"fred"|"mock", "warnings": ...,
//     "chart": null, "extra": {...} }
//
// Reshape from each upstream's native JSON happens inside this TU -- callers
// are not provider-aware. The merged FA / KEY / EE envelopes (Step 6 multi-
// call routes) are produced upstream of this proxy in OmegaApiServer.cpp; this
// proxy just serves the per-component sub-routes.
//
// Mock mode (OMEGA_MARKETDATA_MOCK=1):
//   Bypasses the network entirely and returns canned synthetic JSON in the
//   same shape, with provider="mock". Used for local UI development, CI smoke
//   tests, and Step-7 verification when a host isn't reachable.
//
//   Step-7 hard-cut: OMEGA_OPENBB_MOCK is no longer recognised. If the
//   operator was relying on it, set OMEGA_MARKETDATA_MOCK=1 instead. The
//   value-string parser accepts "1", "true", "TRUE", "yes", "YES" as truthy.
//
// FRED API key (CURV only):
//   Read from OMEGA_FRED_KEY at process start. Free tier @ 120 req/min is
//   plenty for the 11-tenor CURV pull (one request per maturity). If the
//   variable is unset and mock mode is also off, /curv returns HTTP 503 with
//   a structured error body; the other 16 routes work without it.
//
// Threading:
//   Safe for concurrent callers. Cache and curl handles are guarded by an
//   internal mutex; one curl_global_init runs at first use; one curl_easy
//   handle is created per request inside the lock. Per-call handle alloc is
//   negligible against an HTTPS round trip.
//
// Caching:
//   Per-call TTL. Caller specifies how long the response can be cached; pass
//   0 to bypass. Cache key is the full reshape-already URL (route + query),
//   bounded at 256 entries with LRU eviction. The reshaped envelope is what
//   gets cached -- not the raw upstream response -- so existing parsers in
//   OmegaApiServer.cpp / WatchScheduler.cpp don't see provider differences.
//
// Build gate:
//   The .cpp is gated on `#ifndef OMEGA_BACKTEST` -- backtest targets stay
//   free of libcurl. CMake side links CURL::libcurl into the Omega target
//   only. Same shape as the retired OpenBbProxy. The vcpkg/libcurl probes
//   and runtime-DLL POST_BUILD copy in CMakeLists.txt are unchanged in
//   Step 7 since the substrate is still libcurl + OpenSSL.
// ==============================================================================

#ifndef OMEGA_BACKTEST

#include <cstdint>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace omega {

struct MarketDataResult {
    int         status;     // HTTP status (200, 503, 504, ...)
    std::string body;       // OpenBB-OBBject-shaped JSON, or error JSON, or ""
    bool        from_cache; // true if served from the per-URL LRU cache
};

class MarketDataProxy
{
public:
    // Process-wide singleton. First call performs curl_global_init and reads
    // OMEGA_MARKETDATA_MOCK / OMEGA_FRED_KEY from the environment.
    static MarketDataProxy& instance();

    // Dispatches a Step-5/6 OpenBB-style route to the right upstream provider,
    // fetches, reshapes the response into our existing envelope shape, then
    // returns the body via MarketDataResult. ttl_ms gates the per-URL LRU
    // cache; pass 0 to bypass.
    //
    // route: path portion AFTER /api/v1/, e.g. "news/world" or
    //        "equity/price/quote" (preserved from Step 5/6).
    // query: raw query string without the leading '?', e.g.
    //        "symbol=SPY,QQQ&provider=yfinance" (the &provider= suffix is
    //        ignored in Step 7 -- the route alone selects the upstream).
    //
    // Recognised routes (all preserve their Step-5/6 envelope shape):
    //   news/world                                  -> Yahoo search
    //   fixedincome/government/treasury_rates        -> FRED observations
    //   equity/price/quote                          -> Yahoo quote
    //   equity/discovery/{active,gainers,losers}    -> Yahoo screener
    //   derivatives/options/chains                  -> Yahoo options
    //   equity/fundamental/{income,balance,cash}    -> Yahoo quoteSummary
    //   equity/fundamental/{key_metrics,multiples}  -> Yahoo quoteSummary
    //   equity/fundamental/dividends                -> Yahoo chart (events=div)
    //   equity/estimates/{consensus,surprise}       -> Yahoo quoteSummary
    //   news/company                                -> Yahoo search
    //   equity/price/historical                     -> Yahoo chart
    //   equity/profile                              -> Yahoo quoteSummary
    //   currency/price/quote                        -> Yahoo quote (=X form)
    //   crypto/price/quote                          -> Yahoo quote (-USD form)
    //
    // Anything else falls through to a generic "no rows" envelope with
    // provider="omega-stub", same fallthrough behaviour as Step 5/6.
    MarketDataResult get(const std::string& route,
                         const std::string& query,
                         int64_t            ttl_ms);

    // True when OMEGA_MARKETDATA_MOCK=1 (Step 7 hard-cut env var name).
    bool mock_mode() const { return mock_; }

    // True when OMEGA_FRED_KEY was found at startup.
    bool fred_key_present() const { return !fred_key_.empty(); }

    // Returns the upstream provider label for a given Step-5/6 route. Used
    // by WatchScheduler and other engine-side consumers that need to stamp
    // `provider` on a snapshot before any upstream call has happened.
    // Returns "yahoo", "fred", or "omega-stub".
    static const char* upstream_provider_for_route(const std::string& route);

private:
    MarketDataProxy();
    MarketDataProxy(const MarketDataProxy&)            = delete;
    MarketDataProxy& operator=(const MarketDataProxy&) = delete;

    // ---- libcurl HTTP GET --------------------------------------------------
    MarketDataResult http_get(const std::string& url,
                              const std::vector<std::string>& extra_headers);

    // ---- Per-route dispatch ------------------------------------------------
    MarketDataResult dispatch(const std::string& route,
                              const std::string& query);

    // ---- Yahoo Finance fetchers (each reshapes to the legacy envelope) ----
    MarketDataResult yahoo_quote(const std::string& query);
    MarketDataResult yahoo_chart(const std::string& query);
    MarketDataResult yahoo_options(const std::string& query);
    MarketDataResult yahoo_quote_summary(const std::string& route,
                                         const std::string& query);
    MarketDataResult yahoo_search_news(const std::string& query,
                                       bool per_symbol);
    MarketDataResult yahoo_screener(const std::string& route);
    MarketDataResult yahoo_dividends(const std::string& query);
    MarketDataResult yahoo_fx_or_crypto(const std::string& query,
                                        const std::string& kind);

    // ---- FRED treasury-rates ----------------------------------------------
    MarketDataResult fred_treasury_curve(const std::string& query);

    // ---- Mock substrate (same canned JSON as Step 5/6, gated under
    // OMEGA_MARKETDATA_MOCK)
    MarketDataResult fetch_mock(const std::string& route,
                                const std::string& query);

    struct CacheEntry {
        MarketDataResult result;
        int64_t          stored_ms;
        int64_t          ttl_ms;
    };

    std::string fred_key_;
    bool        mock_;

    std::mutex                                   mu_;
    std::unordered_map<std::string, CacheEntry>  cache_;
    std::list<std::string>                       lru_;
    static constexpr size_t                      kMaxCacheEntries = 256;
};

} // namespace omega

#endif // OMEGA_BACKTEST
