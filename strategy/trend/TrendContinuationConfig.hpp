// =============================================================================
// TrendContinuationConfig.hpp - Trend Continuation Strategy Configuration
// =============================================================================
#pragma once

#include <cstdint>

namespace chimera {
namespace strategy {

struct TrendContinuationConfig {
    double min_move;   // minimum price move to confirm trend
};

} // namespace strategy
} // namespace chimera
