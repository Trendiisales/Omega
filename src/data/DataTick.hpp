#pragma once
#include <string>
#include <cstdint>

namespace Omega {

struct DataTick {
    std::string symbol;
    double bid = 0.0;
    double ask = 0.0;
    double mid = 0.0;
    double spread = 0.0;

    double bidSize = 0.0;
    double askSize = 0.0;

    uint64_t tsLocal = 0;
    uint64_t tsExchange = 0;
    
    void compute() {
        mid = (bid + ask) / 2.0;
        spread = ask - bid;
    }
};

} // namespace Omega
