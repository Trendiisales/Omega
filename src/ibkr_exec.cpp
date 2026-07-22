// ibkr_exec.cpp -- the ONLY translation unit that pulls in the TWS API.
//
// Implements the thin omega::ibkr_exec:: interface (IbkrExec.hpp) by wrapping a
// file-static IbkrExecutionEngine. Keeping the TWS headers (and their C
// <stdlib.h> includes) out of the giant main Omega TU avoids the namespace
// pollution that broke std::atoll resolution on the first integration build.
//
// Compiled into Omega.exe only (CMake), where OMEGA_WITH_IBKR is defined. For any
// other target the interface degrades to safe no-ops.
#include "IbkrExec.hpp"

#ifdef OMEGA_WITH_IBKR

#include "IbkrExecutionEngine.hpp"

namespace omega {
namespace ibkr_exec {

static IbkrExecutionEngine& eng() {
    static IbkrExecutionEngine e;   // single instance, constructed on first use
    return e;
}

void configure(const std::string& host, int port, bool paper_only) {
    eng().host = host;
    eng().port = port;
    eng().paper_only = paper_only;
}
void set_enabled(bool on)  { eng().enabled.store(on); }
bool is_enabled()          { return eng().enabled.load(); }
void set_on_fill(std::function<void(const IbkrFill&)> cb) { eng().on_fill = std::move(cb); }
bool connect()             { return eng().connect(); }
void disconnect()          { eng().stop_watchdog(); eng().stop(); }
void start_watchdog()      { eng().ensure_watchdog(); }

long place_order(const std::string& omega_sym, bool is_long, double qty,
                 const std::string& type, double px) {
    return eng().place_order(omega_sym, is_long, qty, type, px);
}

bool is_resolved(const std::string& omega_sym) {
    return eng().is_resolved(omega_sym);
}

} // namespace ibkr_exec
} // namespace omega

#else  // !OMEGA_WITH_IBKR -- safe no-op stubs

namespace omega {
namespace ibkr_exec {
void configure(const std::string&, int, bool) {}
void set_enabled(bool) {}
bool is_enabled() { return false; }
void set_on_fill(std::function<void(const IbkrFill&)>) {}
bool connect() { return false; }
void disconnect() {}
void start_watchdog() {}
long place_order(const std::string&, bool, double, const std::string&, double) { return -1; }
bool is_resolved(const std::string&) { return false; }
} // namespace ibkr_exec
} // namespace omega

#endif
