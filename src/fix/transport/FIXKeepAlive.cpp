#include "FIXKeepAlive.hpp"
#include <chrono>

namespace Omega {

FIXKeepAlive::FIXKeepAlive(FIXTransport* t)
    : tr(t), running(false), interval(1000) {}

FIXKeepAlive::~FIXKeepAlive() {
    stop();
}

void FIXKeepAlive::start(uint64_t intervalMs) {
    interval = intervalMs;
    running = true;

    th = std::thread(&FIXKeepAlive::loop, this);
}

void FIXKeepAlive::stop() {
    running = false;
    if (th.joinable()) th.join();
}

void FIXKeepAlive::loop() {
    using namespace std::chrono;

    const std::string hb =
        "8=FIX.4.4\x01"
        "35=0\x01"
        "112=KA\x01"
        "10=000\x01";   // checksum placeholder

    while (running) {
        std::this_thread::sleep_for(milliseconds(interval));
        if (!running) break;

        if (tr)
            tr->sendRaw(hb);
    }
}

} // namespace Omega
