#pragma once
// =============================================================================
// OmegaTradeLedger.hpp -- BACKTEST STUB
// -----------------------------------------------------------------------------
// Minimal stand-in for the production OmegaTradeLedger header. Provides only
// the omega::TradeRecord struct fields that UsdjpyAsianOpenEngine writes.
// Compiled into the backtest harness via -I backtest/usdjpy_bt taking
// precedence over -I include. The production header is untouched.
// =============================================================================

#include <cstdint>
#include <string>

namespace omega {

struct TradeRecord {
    int         id            = 0;
    std::string symbol;
    std::string side;
    double      entryPrice    = 0.0;
    double      exitPrice     = 0.0;
    double      tp            = 0.0;
    double      sl            = 0.0;
    double      size          = 0.0;
    double      pnl           = 0.0;
    double      net_pnl       = 0.0;
    double      mfe           = 0.0;
    double      mae           = 0.0;
    int64_t     entryTs       = 0;
    int64_t     exitTs        = 0;
    std::string exitReason;
    double      spreadAtEntry = 0.0;
    double      bracket_hi    = 0.0;
    double      bracket_lo    = 0.0;
    std::string engine;
    std::string regime;
    bool        shadow        = false;
};

} // namespace omega
