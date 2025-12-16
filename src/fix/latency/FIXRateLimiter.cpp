#include "FIXRateLimiter.hpp"
#include <thread>

namespace Omega {

FIXRateLimiter::FIXRateLimiter()
    : interval(0),
      lastTs(0),
      rejected(0),
      maxPerSec(0),
      tokens(0),
      lastRefill(0) {}

FIXRateLimiter::~FIXRateLimiter() {}

uint64_t FIXRateLimiter::nowMs() const {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void FIXRateLimiter::setLimit(uint64_t minIntervalMs) {
    std::lock_guard<std::mutex> g(lock);
    interval = minIntervalMs;
}

void FIXRateLimiter::setMaxPerSecond(uint64_t maxOps) {
    std::lock_guard<std::mutex> g(lock);
    maxPerSec = maxOps;
    tokens = maxOps;
    lastRefill = nowMs();
}

bool FIXRateLimiter::allowed() {
    std::lock_guard<std::mutex> g(lock);
    uint64_t now = nowMs();
    
    // Interval-based limiting
    if (interval > 0) {
        if (now - lastTs < interval) {
            rejected++;
            return false;
        }
    }
    
    // Token bucket limiting
    if (maxPerSec > 0) {
        // Refill tokens
        uint64_t elapsed = now - lastRefill;
        if (elapsed >= 1000) {
            tokens = maxPerSec;
            lastRefill = now;
        } else {
            uint64_t refill = (elapsed * maxPerSec) / 1000;
            tokens = std::min(maxPerSec, tokens + refill);
            lastRefill = now;
        }
        
        if (tokens == 0) {
            rejected++;
            return false;
        }
        tokens--;
    }
    
    lastTs = now;
    return true;
}

bool FIXRateLimiter::tryAcquire() {
    return allowed();
}

void FIXRateLimiter::waitUntilAllowed() {
    while (!allowed()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

uint64_t FIXRateLimiter::rejectedCount() const {
    return rejected.load();
}

void FIXRateLimiter::reset() {
    std::lock_guard<std::mutex> g(lock);
    lastTs = 0;
    rejected = 0;
    tokens = maxPerSec;
    lastRefill = nowMs();
}

} // namespace Omega
