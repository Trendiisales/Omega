// IbkrExec.hpp -- THIN interface to the native C++ IBKR execution engine.
//
// Contains NO TWS API headers -- only std types. The main Omega translation unit
// (order_exec.hpp / engine_init.hpp) includes ONLY this. The heavy TWS-API impl
// lives in src/ibkr_exec.cpp (the single TU that pulls in IbkrExecutionEngine.hpp
// + the vendored TWS client). This isolation prevents the TWS C headers
// (<stdlib.h> etc.) from polluting the main TU -- which broke std::atoll
// resolution in unrelated headers on the first integration build (2026-06-16).
//
// All entry points are no-ops returning safe defaults unless OMEGA_WITH_IBKR is
// defined for the compiling target (Omega.exe only).
#pragma once

#include <string>
#include <functional>

namespace omega {

// Reconciled IBKR fill handed to the ledger sink. Plain std types only.
struct IbkrFill {
    std::string omega_symbol;
    std::string ibkr_symbol;
    std::string side;        // "BOT" / "SLD"
    double      qty   = 0.0;
    double      price = 0.0;
    long        order_id = 0;
    std::string exec_id;
    long        ts_unix = 0;
};

namespace ibkr_exec {

// Configure before connect(). port 4002=paper / 4001=live. paper_only refuses
// real orders (and place_order hard-refuses if paper_only && port==4001).
void configure(const std::string& host, int port, bool paper_only);
void set_enabled(bool on);
bool is_enabled();

// Set the fill sink (-> OmegaTradeLedger). Plain std::function, no TWS types.
void set_on_fill(std::function<void(const IbkrFill&)> cb);

// Connect + login to the gateway, start the reader/pump thread, qualify contracts.
bool connect();
void disconnect();

// Start the reconnect watchdog: retries connect() while enabled && disconnected,
// so a boot-race (gateway not up yet) or a mid-session socket drop self-recovers
// without a full Omega restart. Idempotent -- safe to call once after connect().
void start_watchdog();

// Route an order. type: "MKT"|"LMT"|"STP". Returns IBKR orderId, or -1 if rejected
// (disabled / not connected / unresolved contract / paper_only-on-live-port).
long place_order(const std::string& omega_sym, bool is_long, double qty,
                 const std::string& type = "MKT", double px = 0.0);

// True once the omega symbol has a qualified (front-month-resolved) contract.
// Used by [EXEC-SMOKE] to wait for qualification before firing. Stub: false.
bool is_resolved(const std::string& omega_sym);

// Print every open order on the account ([IBKR-EXEC] OPEN-ORDER lines), then
// OPEN-ORDER-END. Stub: no-op.
void list_open_orders();

// reqGlobalCancel -- cancels ALL open orders on the account (every client id).
// Operator-ordered orphan-order cleanup only. Stub: no-op.
void cancel_all_orders();

// [EXEC-PREFLIGHT] whatIf margin/permission check -- prints VIABLE (+margin,
// commission) or the broker error per symbol; NOTHING executes. Stub: -1.
long preflight(const std::string& omega_sym, bool is_long, double qty);

// True if the given preflight oid was answered with err 201/460 (margin or
// permission reject). Poll a few seconds after preflight(). Stub: true.
bool preflight_rejected(long oid);

// Live position marks (S-23a): stream last-price for a symbol through the exec
// connection (delayed OK) / read the latest. Stubs: no-op / 0.0.
void   ensure_mktdata(const std::string& omega_sym);
double last_price(const std::string& omega_sym);

} // namespace ibkr_exec
} // namespace omega
