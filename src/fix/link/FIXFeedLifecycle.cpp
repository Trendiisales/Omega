#include "FIXFeedLifecycle.hpp"
#include "../FIXMessage.hpp"
#include <chrono>
#include <set>

namespace Omega {

FIXFeedLifecycle::FIXFeedLifecycle(FIXSession* s, FIXMDSubscription* m)
    : sess(s), sub(m), running(false) {}

FIXFeedLifecycle::~FIXFeedLifecycle() {
    stop();
}

void FIXFeedLifecycle::start() {
    running = true;
    th = std::thread(&FIXFeedLifecycle::loop, this);
}

void FIXFeedLifecycle::stop() {
    running = false;
    if (th.joinable()) th.join();
}

void FIXFeedLifecycle::add(const std::string& sym) {
    std::lock_guard<std::mutex> g(lock);
    watch.insert(sym);
    if (sub) sub->subscribe(sym);
}

void FIXFeedLifecycle::remove(const std::string& sym) {
    std::lock_guard<std::mutex> g(lock);
    watch.erase(sym);
    if (sub) sub->unsubscribe(sym);
}

void FIXFeedLifecycle::loop() {
    using namespace std::chrono;
    while (running) {
        std::this_thread::sleep_for(seconds(10));

        // Send heartbeat to keep session alive
        if (sess) {
            sess->sendMessage("35=0|");  // Heartbeat
        }
    }
}

} // namespace Omega
