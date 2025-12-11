#pragma once
#include <thread>
#include <atomic>
#include <cstdint>
#include <string>
#include "FIXTransport.hpp"

namespace Omega {

class FIXKeepAlive {
public:
    FIXKeepAlive(FIXTransport* t);
    ~FIXKeepAlive();

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
