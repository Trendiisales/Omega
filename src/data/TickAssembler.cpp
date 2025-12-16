#include "TickAssembler.hpp"
#include <chrono>

namespace Omega {

static uint64_t local_ts() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

TickAssembler::TickAssembler() {}
TickAssembler::~TickAssembler() {}

void TickAssembler::setCallback(std::function<void(const UnifiedTick&)> cb) {
    onTick = cb;
}

void TickAssembler::pushFIX(double bid, double ask, double bidSz, double askSz, uint64_t ts) {
    std::lock_guard<std::mutex> lock(mtx);
    
    lastTick.bid = bid;
    lastTick.ask = ask;
    lastTick.bidSize = bidSz;
    lastTick.askSize = askSz;
    lastTick.spread = (ask > 0 && bid > 0) ? (ask - bid) : 0;
    lastTick.tsExchange = ts;
    lastTick.tsLocal = local_ts();
    
    if (onTick) onTick(lastTick);
}

void TickAssembler::pushBinance(const BinanceTick& t) {
    std::lock_guard<std::mutex> lock(mtx);
    
    lastTick.symbol = t.symbol;
    lastTick.bid = t.bid;
    lastTick.ask = t.ask;
    lastTick.bidSize = t.bidSize;
    lastTick.askSize = t.askSize;
    lastTick.spread = (t.ask > 0 && t.bid > 0) ? (t.ask - t.bid) : 0;
    lastTick.tsExchange = t.ts;
    lastTick.tsLocal = local_ts();
    
    if (onTick) onTick(lastTick);
}

UnifiedTick TickAssembler::last() const {
    std::lock_guard<std::mutex> lock(mtx);
    return lastTick;
}

} // namespace Omega
