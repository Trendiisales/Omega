#include "FIXMDOrderBook.hpp"
#include <algorithm>
#include <chrono>

namespace Omega {

static uint64_t ts_now() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

FIXMDOrderBook::FIXMDOrderBook()
    : ts(0)
{
}

FIXMDOrderBook::~FIXMDOrderBook() {}

void FIXMDOrderBook::updateBid(double price, double size) {
    std::lock_guard<std::mutex> lock(mtx);

    auto it = std::find_if(bids_.begin(), bids_.end(),
        [&](auto& x){ return x.price == price; });

    if (it == bids_.end()) {
        bids_.push_back({price, size});
    } else {
        it->size = size;
    }

    std::sort(bids_.begin(), bids_.end(),
              [](auto& a, auto& b){ return a.price > b.price; });

    ts = ts_now();
}

void FIXMDOrderBook::updateAsk(double price, double size) {
    std::lock_guard<std::mutex> lock(mtx);

    auto it = std::find_if(asks_.begin(), asks_.end(),
        [&](auto& x){ return x.price == price; });

    if (it == asks_.end()) {
        asks_.push_back({price, size});
    } else {
        it->size = size;
    }

    std::sort(asks_.begin(), asks_.end(),
              [](auto& a, auto& b){ return a.price < b.price; });

    ts = ts_now();
}

void FIXMDOrderBook::clear() {
    std::lock_guard<std::mutex> lock(mtx);
    bids_.clear();
    asks_.clear();
    ts = ts_now();
}

FIXMDBook FIXMDOrderBook::snapshot() const {
    std::lock_guard<std::mutex> lock(mtx);
    FIXMDBook b;
    b.bids = bids_;
    b.asks = asks_;
    b.ts = ts;
    return b;
}

double FIXMDOrderBook::bestBid() const {
    std::lock_guard<std::mutex> lock(mtx);
    return bids_.empty() ? 0.0 : bids_.front().price;
}

double FIXMDOrderBook::bestAsk() const {
    std::lock_guard<std::mutex> lock(mtx);
    return asks_.empty() ? 0.0 : asks_.front().price;
}

double FIXMDOrderBook::bestBidSize() const {
    std::lock_guard<std::mutex> lock(mtx);
    return bids_.empty() ? 0.0 : bids_.front().size;
}

double FIXMDOrderBook::bestAskSize() const {
    std::lock_guard<std::mutex> lock(mtx);
    return asks_.empty() ? 0.0 : asks_.front().size;
}

} // namespace Omega
