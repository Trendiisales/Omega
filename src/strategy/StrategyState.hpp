#pragma once
#include <cstdint>

namespace Omega {

struct StrategyState {
    double v1 = 0.0;
    double v2 = 0.0;
    double v3 = 0.0;
    double lastSignal = 0.0;
    uint64_t ts = 0;
    
    void reset() {
        v1 = v2 = v3 = lastSignal = 0.0;
        ts = 0;
    }
};

} // namespace Omega
