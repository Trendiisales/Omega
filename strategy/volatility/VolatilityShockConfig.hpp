// =============================================================================
// VolatilityShockConfig.hpp - Volatility Shock Strategy Configuration
// =============================================================================
#pragma once

#include <cstdint>

namespace chimera {
namespace strategy {

struct VolatilityShockConfig {
    double shock;   // minimum price move to trigger
};

} // namespace strategy
} // namespace chimera
