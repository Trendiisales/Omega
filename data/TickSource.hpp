// =============================================================================
// TickSource.hpp - Common Tick Source Interface
// =============================================================================
// Produces CanonicalTick objects. Implementation decides source.
// Cold-path only - virtual dispatch acceptable here.
// =============================================================================
#pragma once

#include <cstdint>
#include "market/MarketTypes.hpp"

namespace chimera {
namespace data {

class TickSource {
public:
    using Tick = chimera::market::Tick;

    virtual ~TickSource() = default;

    // Returns false when no more ticks
    virtual bool next(Tick& out) noexcept = 0;

    // Reset to beginning (for replay)
    virtual void reset() noexcept {}

    // Total tick count (if known)
    virtual std::size_t size() const noexcept { return 0; }
};

} // namespace data
} // namespace chimera
