#include "FIXDepthBuilder.hpp"
#include <chrono>

namespace Omega {

static uint64_t db_ts() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

FIXDepthBuilder::FIXDepthBuilder()
    : depthLimit(10), ob(nullptr) {}

FIXDepthBuilder::~FIXDepthBuilder() {}

void FIXDepthBuilder::setDepthLimit(size_t levels) {
    depthLimit = levels;
}

void FIXDepthBuilder::attachBook(FIXMDOrderBook* book) {
    ob = book;
}

DepthSnapshot FIXDepthBuilder::build(const std::string& symbol) {
    DepthSnapshot snap;
    snap.symbol = symbol;
    snap.ts = db_ts();

    if (!ob) return snap;

    const auto& bids = ob->bids();
    const auto& asks = ob->asks();

    snap.bids.reserve(depthLimit);
    snap.bidSizes.reserve(depthLimit);
    snap.asks.reserve(depthLimit);
    snap.askSizes.reserve(depthLimit);

    size_t count = 0;
    for (const auto& level : bids) {
        if (count >= depthLimit) break;
        snap.bids.push_back(level.price);
        snap.bidSizes.push_back(level.size);
        ++count;
    }

    count = 0;
    for (const auto& level : asks) {
        if (count >= depthLimit) break;
        snap.asks.push_back(level.price);
        snap.askSizes.push_back(level.size);
        ++count;
    }

    return snap;
}

} // namespace Omega
