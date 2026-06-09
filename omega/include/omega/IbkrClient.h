// omega/include/omega/IbkrClient.h
// Thin synchronous adapter over the Interactive Brokers TWS API (C++).
//
// Compiled ONLY when OMEGA_WITH_IBKR is defined and the TWS API headers are on
// the include path. The rest of Omega (loader, indicators, strategy, backtester,
// scanner) has zero IBKR dependency, so you can develop and backtest from CSV
// without TWS running, then flip this on to pull live history from IB Gateway
// or Trader Workstation.
//
// Build example (after installing the TWS API C++ source, e.g. ~/twsapi):
//   g++ -std=c++17 -DOMEGA_WITH_IBKR
//       -I include -I $TWSAPI/source/cppclient/client
//       src/*.cpp $TWSAPI/source/cppclient/client/*.cpp
//       -lpthread -o omega_ibkr_fetch
// (see README "IBKR build" for the exact, copy-pasteable command)
#ifndef OMEGA_IBKRCLIENT_H
#define OMEGA_IBKRCLIENT_H

#include <string>
#include "omega/Bar.h"

namespace omega {

struct IbkrConfig {
    std::string host = "127.0.0.1";
    int port = 7497;        // 7497 paper TWS, 7496 live TWS, 4002/4001 IB Gateway
    int clientId = 11;
    int timeoutSec = 30;
};

// Parameters for a single historical-data request (maps to reqHistoricalData).
struct HistRequest {
    std::string symbol;
    std::string secType   = "STK";
    std::string exchange  = "SMART";
    std::string currency  = "USD";
    std::string endDateTime = "";        // "" = now
    std::string durationStr = "2 Y";     // e.g. "2 Y", "6 M", "90 D"
    std::string barSize     = "1 day";   // e.g. "1 day", "1 hour", "5 mins"
    std::string whatToShow  = "TRADES";  // TRADES / ADJUSTED_LAST / MIDPOINT
    bool useRTH = true;                  // regular trading hours only
};

#ifdef OMEGA_WITH_IBKR

// Connects to TWS/IB Gateway and pulls bars synchronously. One request at a
// time keeps it deterministic and easy to reason about; loop over symbols for
// a universe download.
class IbkrClient {
public:
    explicit IbkrClient(IbkrConfig cfg);
    ~IbkrClient();

    bool connect();
    void disconnect();

    // Blocking historical fetch. Returns a Series (oldest-first). Throws
    // std::runtime_error on connection/API error or timeout.
    Series fetchHistorical(const HistRequest& req);

private:
    struct Impl;
    Impl* impl_;  // pImpl hides the TWS API types from this header's users
};

#else  // !OMEGA_WITH_IBKR

// Stub so code can include this header in CSV-only builds. Any use throws.
class IbkrClient {
public:
    explicit IbkrClient(IbkrConfig) {}
    bool connect() { fail(); return false; }
    void disconnect() {}
    Series fetchHistorical(const HistRequest&) { fail(); return Series{}; }
private:
    [[noreturn]] static void fail();
};

#endif // OMEGA_WITH_IBKR

} // namespace omega

#endif // OMEGA_IBKRCLIENT_H
