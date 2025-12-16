// =============================================================================
// Intent.hpp - Strategy Output / Execution Input
// =============================================================================
// Fixed-size, no allocation.
// This is the contract between strategies and execution.
// =============================================================================
#pragma once

#include <cstdint>

namespace chimera {
namespace engine {

// =============================================================================
// Intent
// =============================================================================
struct Intent {
    uint32_t symbol_id;
    uint8_t  side;      // 0=buy, 1=sell
    uint8_t  _pad[3];
    uint16_t venue;     // target venue
    uint16_t _pad2;
    double   price;
    double   size;
    uint64_t ts_ns;     // timestamp when intent was created
};

static_assert(sizeof(Intent) == 40, "Intent size must be fixed");

// Side constants
constexpr uint8_t INTENT_BUY  = 0;
constexpr uint8_t INTENT_SELL = 1;

} // namespace engine
} // namespace chimera
