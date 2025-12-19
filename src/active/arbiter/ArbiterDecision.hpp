// =============================================================================
// ArbiterDecision.hpp - Arbiter Output
// =============================================================================
// PROPERTIES:
//   - POD
//   - No heap
//   - Deterministic
// =============================================================================
#pragma once
#include <cstdint>

namespace Chimera {

enum class ArbiterVenue : uint8_t {
    Binance,
    CTrader,
    None
};

struct ArbiterDecision {
    ArbiterVenue venue;      // selected venue
    bool         approved;   // execution allowed?
    
    // Helpers
    inline bool shouldExecute() const noexcept { return approved && venue != ArbiterVenue::None; }
    inline bool isBinance() const noexcept { return venue == ArbiterVenue::Binance; }
    inline bool isCTrader() const noexcept { return venue == ArbiterVenue::CTrader; }
};

// Compile-time verification
static_assert(sizeof(ArbiterDecision) <= 8, "ArbiterDecision must be tiny");

} // namespace Chimera
