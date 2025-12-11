#pragma once
#include <atomic>
#include <thread>
#include <cstdint>
#include "FIXTransport.hpp"

namespace Omega {

class FIXSocketMonitor {
public:
    FIXSocketMonitor(FIXTransport* t);

    void start(uint64_t intervalMs);
    void stop();

private:
    void loop();

private:
    FIXTransport* tr;
    std::thread th;
    std::atomic<bool> running;
    uint64_t interval;
};

} // namespace Omega
