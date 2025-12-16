#include "VolatilityBurst.hpp"
#include <cmath>
#include <numeric>

namespace Omega {

VolatilityBurst::VolatilityBurst()
    : window(30), threshold(2.0) {}

VolatilityBurst::~VolatilityBurst() {}

void VolatilityBurst::setWindow(size_t n) {
    std::lock_guard<std::mutex> g(lock);
    window = n;
}

void VolatilityBurst::setThreshold(double t) {
    std::lock_guard<std::mutex> g(lock);
    threshold = t;
}

void VolatilityBurst::add(double mid, uint64_t ts) {
    std::lock_guard<std::mutex> g(lock);
    
    mids.push_back(mid);
    tss.push_back(ts);

    while (mids.size() > window) {
        mids.pop_front();
        tss.pop_front();
    }
}

void VolatilityBurst::reset() {
    std::lock_guard<std::mutex> g(lock);
    mids.clear();
    tss.clear();
}

VolBurst VolatilityBurst::compute() const {
    std::lock_guard<std::mutex> g(lock);
    
    VolBurst v;

    if (mids.size() < 5) return v;

    double sum = std::accumulate(mids.begin(), mids.end(), 0.0);
    double mean = sum / mids.size();

    double var = 0.0;
    for (double x : mids) {
        double d = x - mean;
        var += d * d;
    }
    var /= mids.size();

    double sigma = std::sqrt(var);
    double last = mids.back();
    double prev = mids[mids.size() - 2];

    v.sigma = sigma;
    v.burst = std::fabs(last - prev) / (sigma + 1e-9);
    v.zscore = (last - mean) / (sigma + 1e-9);
    v.ts = tss.back();

    return v;
}

bool VolatilityBurst::isBursting() const {
    VolBurst v = compute();
    return v.burst >= threshold;
}

} // namespace Omega
