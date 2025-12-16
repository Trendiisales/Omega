// =============================================================================
// MarketTypes.hpp - Central Market Data Include
// =============================================================================
// Market contract:
//
// - CanonicalTick is the ONLY hot-path market data structure
// - OrderBook / Snapshot are DERIVED ONLY
// - Engines and strategies must NOT include anything else
//
// Rule: Everything includes MarketTypes.hpp, never CanonicalTick.hpp directly.
// This prevents future "just one more tick type".
// =============================================================================
#pragma once

#include "CanonicalTick.hpp"

namespace chimera {
namespace market {

// Single alias - this is the ONLY tick type
using Tick = CanonicalTick;

// Venue IDs (pre-defined)
enum VenueId : uint16_t {
    VENUE_UNKNOWN  = 0,
    VENUE_BINANCE  = 1,
    VENUE_CTRADER  = 2,
    VENUE_FIX      = 3
};

} // namespace market
} // namespace chimera
