#pragma once
#include <string>
#include <cstdint>
#include <cstring>

namespace Chimera {

struct UnifiedTick {
    char symbol[16] = {0};  // Fixed size for HFT (no heap allocation)

    double bid = 0.0;
    double ask = 0.0;
    double spread = 0.0;

    double bidSize = 0.0;
    double askSize = 0.0;

    double buyVol = 0.0;
    double sellVol = 0.0;

    double delta = 0.0;
    double liquidityGap = 0.0;
    
    // Aggregate depth (sum of levels)
    double bidDepth = 0.0;
    double askDepth = 0.0;

    // Level 2 depth (top 5)
    double b1 = 0.0;
    double b2 = 0.0;
    double b3 = 0.0;
    double b4 = 0.0;
    double b5 = 0.0;

    double a1 = 0.0;
    double a2 = 0.0;
    double a3 = 0.0;
    double a4 = 0.0;
    double a5 = 0.0;

    uint64_t tsExchange = 0;
    uint64_t tsLocal = 0;
    
    // Helpers
    inline double mid() const { return (bid + ask) * 0.5; }
    inline double imbalance() const { 
        double total = bidSize + askSize;
        return total > 0 ? (bidSize - askSize) / total : 0.0;
    }
    inline double depthImbalance() const {
        double total = bidDepth + askDepth;
        return total > 0 ? (bidDepth - askDepth) / total : 0.0;
    }
    
    // Compute aggregate depth from levels
    inline void computeDepth() {
        bidDepth = b1 + b2 + b3 + b4 + b5;
        askDepth = a1 + a2 + a3 + a4 + a5;
    }
};

} // namespace Chimera
