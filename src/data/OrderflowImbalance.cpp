#include "OrderflowImbalance.hpp"
#include <cmath>

namespace Omega {

OrderflowImbalance::OrderflowImbalance()
    : window(30), cumOfi(0) {}

OrderflowImbalance::~OrderflowImbalance() {}

void OrderflowImbalance::setWindow(size_t n) {
    std::lock_guard<std::mutex> g(lock);
    window = n;
}

void OrderflowImbalance::add(double bidVol,
                             double askVol,
                             uint64_t ts)
{
    std::lock_guard<std::mutex> g(lock);
    
    bids.push_back(bidVol);
    asks.push_back(askVol);
    tss.push_back(ts);
    
    cumOfi += (bidVol - askVol);

    while (bids.size() > window) {
        cumOfi -= (bids.front() - asks.front());
        bids.pop_front();
        asks.pop_front();
        tss.pop_front();
    }
}

void OrderflowImbalance::reset() {
    std::lock_guard<std::mutex> g(lock);
    bids.clear();
    asks.clear();
    tss.clear();
    cumOfi = 0;
}

OFIResult OrderflowImbalance::compute() const {
    std::lock_guard<std::mutex> g(lock);
    
    OFIResult r;

    if (bids.empty()) return r;

    double sumB = 0.0;
    double sumA = 0.0;

    for (size_t i = 0; i < bids.size(); ++i) {
        sumB += bids[i];
        sumA += asks[i];
    }

    double denom = (sumB + sumA);
    if (denom > 0) {
        r.ofi = sumB - sumA;
        r.ratio = r.ofi / denom;
    }
    
    r.cumulative = cumOfi;
    r.ts = tss.back();
    return r;
}

} // namespace Omega
