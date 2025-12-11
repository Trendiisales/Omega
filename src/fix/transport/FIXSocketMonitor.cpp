#include "FIXSocketMonitor.hpp"
#include <chrono>

namespace Omega {

FIXSocketMonitor::FIXSocketMonitor(FIXTransport* t)
    : tr(t), running(false), interval(1000) {}

void FIXSocketMonitor::start(uint64_t intervalMs) {
    interval = intervalMs;
    running = true;
    th = std::thread(&FIXSocketMonitor::loop, this);
}

void FIXSocketMonitor::stop() {
    running = false;
    if (th.joinable()) th.join();
}

void FIXSocketMonitor::loop() {
    using namespace std::chrono;
    while (running) {
        std::this_thread::sleep_for(milliseconds(interval));
        if (!running) break;

        // Send a ping (not FIX heartbeat)
        if (tr) tr->sendRaw("PING");
    }
}

} // namespace Omega
