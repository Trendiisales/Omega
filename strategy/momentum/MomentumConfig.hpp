// =============================================================================
// MomentumConfig.hpp - Momentum Strategy Configuration
// =============================================================================
#pragma once

#include <cstdint>

namespace chimera {
namespace strategy {

struct MomentumConfig {
    double threshold;     // price delta threshold
    uint32_t min_ticks;   // minimum ticks before signal
};

} // namespace strategy
} // namespace chimera
