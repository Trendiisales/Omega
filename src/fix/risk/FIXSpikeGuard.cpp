#include "FIXSpikeGuard.hpp"
#include <cmath>
#include <numeric>

namespace Omega {

FIXSpikeGuard::FIXSpikeGuard()
    : window(20),
      threshold(0.05),
      lastSpike(0.0) {}

FIXSpikeGuard::~FIXSpikeGuard() {}

void FIXSpikeGuard::setWindow(size_t n) {
    std::lock_guard<std::mutex> g(lock);
    window = n;
}

void FIXSpikeGuard::setThreshold(double pct) {
    std::lock_guard<std::mutex> g(lock);
    threshold = pct;
}

void FIXSpikeGuard::addValue(double v) {
    std::lock_guard<std::mutex> g(lock);
    
    if (history.size() >= 2) {
        double prev = history.back();
        if (prev != 0.0) {
            double change = std::fabs((v - prev) / prev);
            if (change >= threshold) {
                lastSpike = change;
            } else {
                lastSpike = 0.0;
            }
        }
    }
    
    history.push_back(v);
    while (history.size() > window) {
        history.pop_front();
    }
}

bool FIXSpikeGuard::spikeDetected() const {
    std::lock_guard<std::mutex> g(lock);
    return lastSpike >= threshold && lastSpike > 0.0;
}

double FIXSpikeGuard::lastSpikeMagnitude() const {
    std::lock_guard<std::mutex> g(lock);
    return lastSpike;
}

double FIXSpikeGuard::average() const {
    std::lock_guard<std::mutex> g(lock);
    if (history.empty()) return 0.0;
    double sum = std::accumulate(history.begin(), history.end(), 0.0);
    return sum / history.size();
}

void FIXSpikeGuard::reset() {
    std::lock_guard<std::mutex> g(lock);
    history.clear();
    lastSpike = 0.0;
}

} // namespace Omega
