// BigCapMomoIbkr.hpp -- THIN interface to the in-process IBKR BigCapMomo engine.
//
// Mirrors IbkrExec.hpp deliberately: this header contains NO TWS API headers --
// only std types + the two pure-std Omega structs the telemetry path needs
// (PositionSnapshot, TradeRecord). The heavy TWS-API impl lives in the single TU
// src/bigcap_momo_ibkr.cpp (the only Omega TU besides ibkr_exec.cpp that pulls in
// the vendored TWS client). This isolation keeps the TWS C <stdlib.h> includes
// out of the giant main Omega TU -- the same pollution that broke std::atoll
// resolution on the first IBKR integration build (2026-06-16).
//
// WHY in-process (not the standalone ibkr/BigCapMomoEngine.cpp exe): the Omega
// GUI running-trades panel is in-process telemetry (g_open_positions + shared
// mem) -- a separate exe cannot inject. So the validated big-cap momentum
// continuation engine (SPY-200MA regime gate + day-expansion gate + ignition +
// vol-surge + WIDE trail; see ibkr/BigCapMomoEngine.cpp header for the backtest)
// runs on its OWN IBKR scanner/data thread inside Omega.exe and reports through
// the same telemetry + shadow-ledger path as every other engine.
//
// All entry points are no-ops returning safe defaults unless OMEGA_WITH_IBKR is
// defined for the compiling target (Omega.exe only).
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "OpenPositionRegistry.hpp"   // omega::PositionSnapshot  (pure std types)
#include "OmegaTradeLedger.hpp"       // omega::TradeRecord       (pure std types)

namespace omega {
namespace bigcap_momo_ibkr {

// All knobs are the validated defaults from ibkr/BigCapMomoEngine.cpp / the
// g_bigcap_momo engine_init config. shadow == !paper_only inverse: paper_only
// true => logs only, NO live orders (shadow). port 4001=live data / 4002=paper.
struct Config {
    std::string host        = "127.0.0.1";
    int    port             = 4001;   // IBKR gateway (data); 4002 = paper
    int    client_id        = 86;     // matches the standalone engine's clientId
    bool   paper_only       = true;   // shadow: log trades, route NO live orders
    double gate_pct         = 3.0;    // day-expansion gate: only trade names already +GATE% on session
    double trail_pct        = 0.04;   // 4.0% wide trail off peak (the validated lever)
    double ig_pct           = 3.0;    // ignition: +IG% over LB 5m bars
    double volx             = 3.0;    // volume surge vs 20-bar avg (0 = disable, bridge-delta caveat)
    double px_min           = 10.0;   // big-cap / deep-liquidity floor
    int    lb               = 6;      // ignition lookback (6*5m = 30min)
    int    maxhold          = 48;     // 48*5m = 4h backstop
    bool   regime_gate      = true;   // SPY price>SMA200 AND SMA200 rising
    double notional_usd     = 1000.0; // per-entry notional (shadow sizing)
    std::string engine_tag  = "BigCapMomo";  // ledger + GUI source label
};

// Set config BEFORE start(). Safe to call repeatedly while stopped.
void configure(const Config& cfg);

// Master gate. collect_positions() returns empty and start() refuses to connect
// unless enabled. Lets engine_init register the GUI source unconditionally while
// the runtime decides (env) whether this path is the active one.
void set_enabled(bool on);
bool is_enabled();

// Closed-trade sink -> handle_closed_trade (shadow ledger + telemetry). Plain
// std::function, no TWS types. Set in engine_init alongside the other engines.
void set_on_trade_record(std::function<void(const TradeRecord&)> cb);

// Open positions for g_open_positions.register_source -> the live_trades GUI
// panel. Empty unless enabled + connected + holding. Thread-safe (the impl locks
// its book against the TWS reader thread).
std::vector<PositionSnapshot> collect_positions();

// Connect to the gateway + start the reader/pump thread (the IBKR scanner data
// thread the handoff calls for). Idempotent: a second call while already running
// is a no-op. Returns false if disabled, OMEGA_WITH_IBKR is undefined, or the
// connection failed.
bool start();

// Signal the reader thread to stop, join it, disconnect. Idempotent.
void stop();

} // namespace bigcap_momo_ibkr
} // namespace omega
