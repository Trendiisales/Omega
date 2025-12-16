#include "FIXTcpReconnector.hpp"
#include <chrono>

namespace Omega {

FIXTcpReconnector::FIXTcpReconnector(FIXTransport* t)
    : tr(t), running(false), port(0) {}

void FIXTcpReconnector::setTarget(const std::string& h, int p) {
    host = h;
    port = p;
}

void FIXTcpReconnector::start() {
    running = true;
    th = std::thread(&FIXTcpReconnector::loop, this);
}

void FIXTcpReconnector::stop() {
    running = false;
    if (th.joinable()) th.join();
}

void FIXTcpReconnector::loop() {
    using namespace std::chrono;

    while (running) {
        std::this_thread::sleep_for(milliseconds(policy.nextDelay()));
        if (!running) break;

        if (tr && tr->connect(host, port)) {
            policy.reset();
            return;
        }
    }
}

} // namespace Omega
