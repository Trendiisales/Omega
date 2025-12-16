// =============================================================================
// LiquidityFadeConfig.hpp - Liquidity Fade Strategy Configuration
// =============================================================================
#pragma once

#include <cstdint>

namespace chimera {
namespace strategy {

struct LiquidityFadeConfig {
    double min_size;   // minimum book size to fade
};

} // namespace strategy
} // namespace chimera
