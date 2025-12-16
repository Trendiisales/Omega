#include "LiquidityShockDetector.hpp"
#include <cmath>

namespace Omega {

LiquidityShockDetector::LiquidityShockDetector()
    : window(20), threshold(0.3) {}

LiquidityShockDetector::~LiquidityShockDetector() {}

void LiquidityShockDetector::setWindow(size_t n) {
    std::lock_guard<std::mutex> g(lock);
    window = n;
}

void LiquidityShockDetector::setThreshold(double t) {
    std::lock_guard<std::mutex> g(lock);
    threshold = t;
}

void LiquidityShockDetector::add(double bidDepth,
                                 double askDepth,
                                 uint64_t ts)
{
    std::lock_guard<std::mutex> g(lock);
    
    bids.push_back(bidDepth);
    asks.push_back(askDepth);
    tss.push_back(ts);

    while (bids.size() > window) {
        bids.pop_front();
        asks.pop_front();
        tss.pop_front();
    }
}

void LiquidityShockDetector::reset() {
    std::lock_guard<std::mutex> g(lock);
    bids.clear();
    asks.clear();
    tss.clear();
}

LiquidityShock LiquidityShockDetector::compute() const {
    std::lock_guard<std::mutex> g(lock);
    
    LiquidityShock s;

    if (bids.size() < 3) return s;

    double b1 = bids[bids.size() - 3];
    double b2 = bids[bids.size() - 2];
    double b3 = bids[bids.size() - 1];

    double a1 = asks[asks.size() - 3];
    double a2 = asks[asks.size() - 2];
    double a3 = asks[asks.size() - 1];

    s.bidChange = (b3 - b1) / (b1 + 1e-9);
    s.askChange = (a3 - a1) / (a1 + 1e-9);
    s.depthImpact = s.bidChange - s.askChange;
    s.shock = std::fabs(s.depthImpact);
    s.ts = tss.back();

    return s;
}

bool LiquidityShockDetector::isShocked() const {
    LiquidityShock s = compute();
    return s.shock >= threshold;
}

} // namespace Omega
