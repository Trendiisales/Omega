#include "FIXReconnectPolicy.hpp"

namespace Omega {

FIXReconnectPolicy::FIXReconnectPolicy()
    : minD(100), maxD(5000), backoff(2.0), cur(100) {}

void FIXReconnectPolicy::setDelaysMs(uint64_t minDelay, uint64_t maxDelay) {
    minD = minDelay;
    maxD = maxDelay;
}

void FIXReconnectPolicy::setBackoff(double factor) {
    backoff = factor;
}

uint64_t FIXReconnectPolicy::nextDelay() {
    uint64_t d = cur;
    cur = (uint64_t)(cur * backoff);
    if (cur > maxD) cur = maxD;
    if (d < minD) d = minD;
    if (d > maxD) d = maxD;
    return d;
}

void FIXReconnectPolicy::reset() {
    cur = minD;
}

} // namespace Omega
