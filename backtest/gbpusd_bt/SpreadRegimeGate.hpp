#pragma once
// =============================================================================
// SpreadRegimeGate.hpp -- BACKTEST STUB (GBPUSD)
// -----------------------------------------------------------------------------
// No-op stub matching the production SpreadRegimeGate signature.
// =============================================================================

#include <cstdint>

namespace omega {

class SpreadRegimeGate {
public:
    void on_tick(int64_t /*now_ms*/, double /*spread*/) noexcept {}
    bool can_fire() const noexcept { return true; }
};

} // namespace omega
