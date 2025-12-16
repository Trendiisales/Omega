#pragma once
#include <cstdint>
#include <chrono>
#include <atomic>
#include <mutex>

namespace Omega {

class FIXRateLimiter {
public:
    FIXRateLimiter();
    ~FIXRateLimiter();

    void setLimit(uint64_t minIntervalMs);
    void setMaxPerSecond(uint64_t maxOps);
    
    bool allowed();
    bool tryAcquire();  // Non-blocking check
    void waitUntilAllowed();  // Blocking wait
    
    uint64_t rejectedCount() const;
    void reset();

private:
    uint64_t nowMs() const;

private:
    std::mutex lock;
    uint64_t interval;
    uint64_t lastTs;
    std::atomic<uint64_t> rejected;
    
    // Token bucket for burst handling
    uint64_t maxPerSec;
    uint64_t tokens;
    uint64_t lastRefill;
};

} // namespace Omega
