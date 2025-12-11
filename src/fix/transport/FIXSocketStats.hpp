#pragma once
#include <atomic>
#include <cstdint>

namespace Omega {

struct FIXSocketStats {
    std::atomic<uint64_t> bytesTx;
    std::atomic<uint64_t> bytesRx;
    std::atomic<uint64_t> reconnects;
    std::atomic<uint64_t> heartbeatsTx;
    std::atomic<uint64_t> heartbeatsRx;

    FIXSocketStats();
    void reset();
};

} // namespace Omega
