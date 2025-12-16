// =============================================================================
// MeanReversionConfig.hpp - Mean Reversion Strategy Configuration
// =============================================================================
#pragma once

#include <cstdint>

namespace chimera {
namespace strategy {

struct MeanReversionConfig {
    double deviation;    // deviation from mean threshold
    uint32_t window;     // minimum samples before signal
};

} // namespace strategy
} // namespace chimera
