// =============================================================================
// OrderFlowImbalanceConfig.hpp - Order Flow Imbalance Strategy Configuration
// =============================================================================
#pragma once

#include <cstdint>

namespace chimera {
namespace strategy {

struct OrderFlowImbalanceConfig {
    double ratio;   // buy/sell imbalance ratio threshold
};

} // namespace strategy
} // namespace chimera
