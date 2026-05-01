#pragma once
// ==============================================================================
// WatchScheduler -- cron-style nightly INTEL screener.
//
// Step 6 of the Omega Terminal build. Updated at Step 7 to call
// MarketDataProxy (Yahoo Finance + FRED) instead of the retired OpenBbProxy
// (api.openbb.co returned 404 -- OpenBB does not host a public REST endpoint).
// The scheduler logic, universe constants, screener rule, and on-the-wire
// envelope shape are unchanged; only the upstream HTTP source flips.
//
// Backs the /api/v1/omega/watch route by maintaining an in-process registry
// of "screener hits" -- symbols flagged by the INTEL screener over a configured
// universe (S&P 500 / NDX / ALL) on a nightly schedule. The route handler
// does NOT run the screener; it just snapshots the registry. This keeps
// /watch latency fast and predictable even when the universe is ~520 symbols
// deep.
//
// Threading:
//   start() spawns one worker thread that wakes nightly at 00:30 UTC, runs
//   the scan over each universe, and writes results into the registry under
//   an internal mutex. stop() signals shutdown via a condition_variable and
//   joins. Safe for repeated start()/stop() in shell tests.
//
// Universes:
//   "SP500" -> S&P 500 component list (baked-in constant)
//   "NDX"   -> NASDAQ-100 component list (baked-in constant)
//   "ALL"   -> SP500 + NDX, deduplicated.
//
// Screener (v1):
//   A deliberately-simple momentum + relative-volume screener using
//   MarketDataProxy's /equity/price/quote route (Yahoo Finance under Step 7,
//   batched in groups of 100 to respect provider rate limits). Output: one
//   WatchHit per symbol that passes the criteria. The screener function is
//   a private member so it can be swapped for a richer composite (the
//   existing INTEL panel's server-side rule set) without changing the
//   public surface.
//
// Mock mode:
//   When OMEGA_MARKETDATA_MOCK=1 is set, the scheduler still runs but uses
//   the MarketDataProxy mock data path. This lets the WATCH panel be
//   exercised end-to-end on a dev box without external network access.
//   (Step-7 hard-cut: the legacy OMEGA_OPENBB_MOCK env var is no longer
//   recognised. Operators must use OMEGA_MARKETDATA_MOCK.)
//
// Build gate:
//   The .cpp is gated on `#ifndef OMEGA_BACKTEST` so backtest targets stay
//   free of libcurl. WatchScheduler.cpp uses no third-party deps beyond
//   MarketDataProxy + std::thread + std::condition_variable.
// ==============================================================================

#ifndef OMEGA_BACKTEST

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace omega {

struct WatchHit {
    std::string symbol;
    std::string signal;          // tag, e.g. "MOMO-BO" / "VOL-SPIKE"
    double      score = 0.0;
    double      last_price = 0.0;
    double      change_percent = 0.0;
    double      volume = 0.0;
    int64_t     flagged_at_ms = 0;  // unix ms
    std::string rationale;
};

struct WatchSnapshot {
    std::vector<WatchHit> hits;
    int64_t               last_run_ms = 0;
    int64_t               next_run_ms = 0;
    bool                  scanning    = false;
    std::string           universe;
    // Upstream-provider label. Step 7 values:
    //   "yahoo"      -- live Yahoo Finance quotes (default)
    //   "mock"       -- OMEGA_MARKETDATA_MOCK=1 path
    //   "omega-stub" -- fallthrough (e.g. unknown route)
    // The /watch route serializes this verbatim into the envelope; the UI
    // uses it to render the upper-right MOCK / LIVE-yahoo badge.
    std::string           provider;
};

class WatchScheduler
{
public:
    static WatchScheduler& instance();

    // Spawn the worker thread. Idempotent.
    void start();
    // Signal shutdown and join. Idempotent.
    void stop();

    // Snapshot the current registry for a given universe label.
    // Recognised: "SP500" (default), "NDX", "ALL". Anything else is coerced
    // to "SP500".
    WatchSnapshot snapshot(const std::string& universe);

    // Trigger an immediate scan (useful for ops + dev). Non-blocking; the
    // worker thread processes the request on its next iteration.
    void trigger_now();

    bool running() const { return running_.load(std::memory_order_acquire); }

private:
    WatchScheduler();
    ~WatchScheduler();
    WatchScheduler(const WatchScheduler&)            = delete;
    WatchScheduler& operator=(const WatchScheduler&) = delete;

    void worker_loop();
    void run_scan(const std::string& universe);
    int64_t now_ms() const;
    int64_t next_scan_ms() const;  // 00:30 UTC of tomorrow (or today if before)

    std::atomic<bool>            running_{false};
    std::atomic<bool>            stop_requested_{false};
    std::atomic<bool>            scan_now_{false};
    std::thread                  thread_;
    std::mutex                   mu_;
    std::condition_variable      cv_;

    // Registry: universe -> snapshot. Updated under mu_.
    std::unordered_map<std::string, WatchSnapshot> registry_;
};

} // namespace omega

#endif // OMEGA_BACKTEST
