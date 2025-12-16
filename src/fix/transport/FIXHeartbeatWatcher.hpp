#pragma once
#include <atomic>
#include <thread>
#include <cstdint>
#include <string>
#include "FIXTransport.hpp"

namespace Omega {

class FIXHeartbeatWatcher {
public:
    FIXHeartbeatWatcher(FIXTransport* t);

    void start(uint64_t timeoutMs);
    void stop();

    bool timedOut() const;

private:
    void loop();

private:
    FIXTransport* transport;
    std::atomic<bool> running;
    std::atomic<bool> timeoutFlag;
    uint64_t timeout;
    uint64_t lastSeen;
    std::thread th;
};

} // namespace Omega
