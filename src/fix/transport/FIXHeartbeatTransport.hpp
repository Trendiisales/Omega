#pragma once
#include <thread>
#include <atomic>
#include "FIXTransport.hpp"

namespace Omega {

class FIXHeartbeatTransport {
public:
    FIXHeartbeatTransport(FIXTransport* t);

    void start(uint64_t heartbeatMs);
    void stop();

private:
    void loop();

private:
    FIXTransport* transport;
    std::thread th;
    std::atomic<bool> running;
    uint64_t hb;
};

} // namespace Omega
