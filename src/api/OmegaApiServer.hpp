#pragma once
// ==============================================================================
// OmegaApiServer -- HTTP/1.1 :7781 (all interfaces, since 2026-05-01)
//
// Step 2 of the Omega Terminal build plan (docs/SESSION_2026-05-01_HANDOFF.md).
// Serves a JSON read-API for the new omega-terminal/ React UI. Runs alongside
// the existing src/gui/OmegaTelemetryServer (HTTP :7779 + WS :7780); the two
// are independent — different port, different routes, different audience. The
// telemetry server will retire at the Step 7 cutover; this server stays.
//
// Routes (all GET):
//   /api/v1/omega/engines    -> JSON [Engine]      from g_engines snapshot
//   /api/v1/omega/positions  -> JSON [Position]    open positions across engines
//   /api/v1/omega/ledger     -> JSON [LedgerEntry] closed trades
//   /api/v1/omega/equity     -> JSON [EquityPoint] equity time-series
//
// Threading model:
//   thread_ -- HTTP accept loop (read-only access to g_engines + g_omegaLedger
//              via their own internal mutexes; no shared snap_ pointer needed).
//   running_ is std::atomic<bool> -- correct for cross-thread stop signal.
//
// Bind: 0.0.0.0 (all interfaces) since 2026-05-01 -- the server now also
// serves the omega-terminal React build at /, so the UI is reachable in any
// browser at http://VPS_IP:7781/. Security parity with the legacy
// OmegaTelemetryServer which has been publicly bound forever; restrict
// source IPs via a Windows Firewall rule on :7781 if needed. The server is
// gated off in backtest builds (#ifndef OMEGA_BACKTEST in
// OmegaApiServer.cpp).
//
// Implementation idioms mirror OmegaTelemetryServer.cpp: hand-rolled BSD
// sockets + Winsock, no third-party HTTP/JSON libs. Same SO_REUSEADDR +
// SO_RCVTIMEO=200ms accept-loop pattern so closesocket() in stop() reliably
// wakes a blocked accept().
// ==============================================================================

#include <atomic>
#include <thread>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#endif

namespace omega {

// Step 3 Omega Terminal: equity-anchor setter for the /api/v1/omega/equity
// route. The route walks g_omegaLedger.snapshot() and emits
// equity_at_t = anchor + cumulative_net_pnl(<= t). Default anchor is 10000.0;
// init_engines() in engine_init.hpp calls this with g_cfg.account_equity once
// the config has been loaded so the absolute equity values match the live
// account. Idempotent and lock-free (atomic store).
void set_equity_anchor(double anchor);

class OmegaApiServer
{
public:
    OmegaApiServer();
    ~OmegaApiServer();

    // Start the HTTP accept loop on http_port (default caller passes 7781).
    // Idempotent: a second call while already running is a no-op.
    void start(int http_port);

    // Stop the HTTP accept loop. Safe to call from any thread; blocks until
    // the accept thread has joined.
    void stop();

private:
    void run(int port);

    std::atomic<bool> running_;
    std::thread       thread_;

#ifdef _WIN32
    SOCKET            server_fd_;
#else
    int               server_fd_;
#endif
};

} // namespace omega
