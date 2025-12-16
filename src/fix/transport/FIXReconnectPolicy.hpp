#pragma once
#include <cstdint>
#include <chrono>

namespace Omega {

class FIXReconnectPolicy {
public:
    FIXReconnectPolicy();

    void setDelaysMs(uint64_t minDelay, uint64_t maxDelay);
    void setBackoff(double factor);

    uint64_t nextDelay();

    void reset();

private:
    uint64_t minD;
    uint64_t maxD;
    double backoff;
    uint64_t cur;
};

} // namespace Omega
