#include "FIXHeartbeatTransport.hpp"
#include <chrono>

namespace Omega {

FIXHeartbeatTransport::FIXHeartbeatTransport(FIXTransport* t)
    : transport(t), running(false), hb(1000) {}

void FIXHeartbeatTransport::start(uint64_t heartbeatMs) {
    hb = heartbeatMs;
    running = true;
    th = std::thread(&FIXHeartbeatTransport::loop, this);
}

void FIXHeartbeatTransport::stop() {
    running = false;
    if (th.joinable()) th.join();
}

void FIXHeartbeatTransport::loop() {
    using namespace std::chrono;
    while (running) {
        std::this_thread::sleep_for(milliseconds(hb));
        if (!running) break;

        if (transport)
            transport->sendRaw("8=FIX.4.4|35=0|49=OMEGA|56=DEST|34=1|52=20240101-00:00:00.000|10=000|");
    }
}

} // namespace Omega
