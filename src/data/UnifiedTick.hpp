#pragma once
#include <string>
#include <cstdint>

namespace Omega {

struct UnifiedTick {
    std::string symbol;

    double bid = 0.0;
    double ask = 0.0;
    double spread = 0.0;

    double bidSize = 0.0;
    double askSize = 0.0;

    double buyVol = 0.0;
    double sellVol = 0.0;

    double delta = 0.0;
    double liquidityGap = 0.0;

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
    double mid() const { return (bid + ask) / 2.0; }
    double imbalance() const { 
        double total = bidSize + askSize;
        return total > 0 ? (bidSize - askSize) / total : 0.0;
    }
};

} // namespace Omega
