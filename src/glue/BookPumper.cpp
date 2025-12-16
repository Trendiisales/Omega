#include "BookPumper.hpp"
#include <chrono>

namespace Omega {

BookPumper::BookPumper()
    : book(nullptr),
      running(false)
{
}

BookPumper::~BookPumper() {
    stop();
}

void BookPumper::attachBook(FIXMDOrderBook* b) {
    book = b;
}

void BookPumper::setSymbol(const std::string& s) {
    symbol = s;
}

void BookPumper::setCallback(std::function<void(const UnifiedTick&)> cb) {
    onTick = cb;
}

void BookPumper::start() {
    if (running) return;
    running = true;
    worker = std::thread(&BookPumper::loop, this);
}

void BookPumper::stop() {
    running = false;
    if (worker.joinable()) worker.join();
}

void BookPumper::loop() {
    while (running) {
        if (book) {
            FIXMDBook snap = book->snapshot();
            UnifiedTick ut = normalizer.normalize(snap, symbol);
            if (onTick) onTick(ut);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

} // namespace Omega
