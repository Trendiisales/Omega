#include "FIXMDNormalizer.hpp"
#include <chrono>

namespace Omega {

static uint64_t ts_local_now() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

FIXMDNormalizer::FIXMDNormalizer() {}
FIXMDNormalizer::~FIXMDNormalizer() {}

UnifiedTick FIXMDNormalizer::normalize(const FIXMDBook& b,
                                       const std::string& symbol)
{
    UnifiedTick t;
    t.symbol = symbol;

    if (!b.bids.empty()) {
        t.bid = b.bids.front().price;
        t.bidSize = b.bids.front().size;
    }

    if (!b.asks.empty()) {
        t.ask = b.asks.front().price;
        t.askSize = b.asks.front().size;
    }

    if (t.ask > 0)
        t.spread = t.ask - t.bid;

    t.tsExchange = b.ts;
    t.tsLocal = ts_local_now();

    return t;
}

} // namespace Omega
