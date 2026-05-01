#pragma once
// ==============================================================================
// OpenBbProxy -- libcurl-based HTTPS client for OpenBB Hub
//
// Step 5 of the Omega Terminal build. Backs the
//
//   /api/v1/omega/intel
//   /api/v1/omega/curv
//   /api/v1/omega/wei
//   /api/v1/omega/mov
//
// routes in OmegaApiServer.cpp by forwarding to https://api.openbb.co/api/v1/
// with a Bearer token. Token is read from the OMEGA_OPENBB_TOKEN env var at
// first use. If the variable is unset and OMEGA_OPENBB_MOCK is also unset, the
// proxy returns HTTP 503 with a small JSON error body so the UI can surface a
// clear "API token not configured" message via its standard red retry banner.
//
// Mock mode (OMEGA_OPENBB_MOCK=1):
//   The proxy bypasses the network and returns canned synthetic JSON shaped
//   like an OpenBB OBBject (`{"results":[...], "provider":"mock"}`). Used for
//   local UI development, CI smoke tests, and Step-5 verification when a real
//   OpenBB account is not available. Mock mode is independent of the token --
//   if both are set, MOCK wins.
//
// Threading:
//   Safe for concurrent callers. The cache and curl handles are guarded by an
//   internal mutex; one curl_global_init runs at first use; one curl_easy
//   handle is created per request inside the lock. The cost of allocating a
//   handle per call is negligible against an HTTPS round trip.
//
// Caching:
//   Per-call TTL. The caller specifies how long the response can be cached;
//   pass 0 to bypass. The cache is keyed on the full URL (path + query) and
//   bounded at 256 entries with LRU eviction. This smooths bursty UI polling
//   (e.g. the MOV panel's 1 s cadence) without coalescing distinct callers.
//
// Build:
//   This translation unit is gated on `#ifndef OMEGA_BACKTEST` in the .cpp,
//   matching OmegaApiServer.cpp. The CMake side adds find_package(CURL
//   REQUIRED) and links CURL::libcurl into the Omega target only -- backtest
//   targets do not need market data and stay free of the libcurl dep.
// ==============================================================================

#ifndef OMEGA_BACKTEST

#include <cstdint>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>

namespace omega {

struct OpenBbResult {
    int         status;     // HTTP status (e.g. 200, 503, 504)
    std::string body;       // Response body (JSON, error JSON, or "")
    bool        from_cache; // True when the response came from the LRU cache
};

class OpenBbProxy
{
public:
    // Process-wide singleton. First call performs curl_global_init and reads
    // OMEGA_OPENBB_TOKEN / OMEGA_OPENBB_MOCK from the environment.
    static OpenBbProxy& instance();

    // GET https://api.openbb.co/api/v1/<route>?<query>. Returns the body and
    // status. ttl_ms specifies how long this response can stay in the per-URL
    // cache; pass 0 to bypass the cache.
    //
    // route: path portion AFTER /api/v1/, e.g. "news/world" or
    //        "equity/price/quote".
    // query: raw query string without the leading '?', e.g.
    //        "symbol=SPY,QQQ&provider=fmp".
    OpenBbResult get(const std::string& route,
                     const std::string& query,
                     int64_t            ttl_ms);

    // Returns true if a real OpenBB token was found at startup. Useful for
    // route handlers to short-circuit and return a structured error instead
    // of attempting a guaranteed-401 round trip.
    bool token_present() const { return !token_.empty(); }

    // Returns true if mock mode is active (OMEGA_OPENBB_MOCK=1).
    bool mock_mode() const { return mock_; }

private:
    OpenBbProxy();
    OpenBbProxy(const OpenBbProxy&)            = delete;
    OpenBbProxy& operator=(const OpenBbProxy&) = delete;

    OpenBbResult fetch_remote(const std::string& url);
    OpenBbResult fetch_mock(const std::string& route,
                            const std::string& query);

    struct CacheEntry {
        OpenBbResult result;
        int64_t      stored_ms;
        int64_t      ttl_ms;
    };

    std::string token_;
    bool        mock_;

    std::mutex                                  mu_;
    std::unordered_map<std::string, CacheEntry> cache_;
    std::list<std::string>                      lru_;
    static constexpr size_t                     kMaxCacheEntries = 256;
};

} // namespace omega

#endif // OMEGA_BACKTEST
