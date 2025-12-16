#include "FIXHeartbeatWatcher.hpp"
#include <chrono>

namespace Omega {

FIXHeartbeatWatcher::FIXHeartbeatWatcher(FIXTransport* t)
    : transport(t), running(false), timeoutFlag(false), timeout(3000), lastSeen(0) {}

void FIXHeartbeatWatcher::start(uint64_t timeoutMs) {
    timeout = timeoutMs;
    timeoutFlag = false;
    running = true;

    transport->setRxCallback([this](const std::string& msg) {
        // if message type = heartbeat(0), update timestamp
        if (msg.find("35=0") != std::string::npos) {
            lastSeen = (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count();
        }
    });

    th = std::thread(&FIXHeartbeatWatcher::loop, this);
}

void FIXHeartbeatWatcher::stop() {
    running = false;
    if (th.joinable()) th.join();
}

bool FIXHeartbeatWatcher::timedOut() const {
    return timeoutFlag.load();
}

void FIXHeartbeatWatcher::loop() {
    using namespace std::chrono;

    lastSeen = (uint64_t)steady_clock::now().time_since_epoch().count();

    while (running) {
        std::this_thread::sleep_for(milliseconds(200));
        if (!running) break;

        uint64_t now = (uint64_t)steady_clock::now().time_since_epoch().count();
        uint64_t deltaMs = (now - lastSeen) / 1'000'000;

        if (deltaMs > timeout) {
            timeoutFlag = true;
            break;
        }
    }
}

} // namespace Omega
