#pragma once
#include <cstdint>
#include <cstddef>
#include <array>

namespace Omega {

struct MicroState {
    double gradient = 0.0;
    double accel = 0.0;
    double pressure = 0.0;
    double wave = 0.0;
    double ofi = 0.0;
    double imbalance = 0.0;
    double volatility = 0.0;
    double momentum = 0.0;
    double trend = 0.0;
    double reversion = 0.0;
    double breakout = 0.0;
    double volume = 0.0;
    double liquidity = 0.0;
    double impact = 0.0;
    double spread = 0.0;
    double depth = 0.0;
    double flow = 0.0;
    double regime = 0.0;
    double signal = 0.0;
    double confidence = 0.0;
    uint64_t ts = 0;
    
    std::array<double, 32> v = {};
    
    double& operator[](std::size_t i) { return v[i]; }
    double operator[](std::size_t i) const { return v[i]; }
};

} // namespace Omega
