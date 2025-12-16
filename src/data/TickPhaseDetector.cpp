#include "TickPhaseDetector.hpp"
#include <cmath>

namespace Omega {

TickPhaseDetector::TickPhaseDetector()
    : window(20) {}

TickPhaseDetector::~TickPhaseDetector() {}

void TickPhaseDetector::setWindow(size_t n) {
    std::lock_guard<std::mutex> g(lock);
    window = n;
}

void TickPhaseDetector::add(double mid, uint64_t ts) {
    std::lock_guard<std::mutex> g(lock);
    
    mids.push_back(mid);
    tss.push_back(ts);

    while (mids.size() > window) {
        mids.pop_front();
        tss.pop_front();
    }
}

void TickPhaseDetector::reset() {
    std::lock_guard<std::mutex> g(lock);
    mids.clear();
    tss.clear();
}

TickPhase TickPhaseDetector::compute() const {
    std::lock_guard<std::mutex> g(lock);
    
    TickPhase p;

    if (mids.size() < 3) return p;

    double m1 = mids[mids.size()-3];
    double m2 = mids[mids.size()-2];
    double m3 = mids[mids.size()-1];

    double v1 = m2 - m1;
    double v2 = m3 - m2;

    p.impulse = m3 - m1;
    p.direction = (p.impulse == 0 ? 0 : (p.impulse > 0 ? 1 : -1));
    p.velocity = v2;
    p.acceleration = v2 - v1;
    p.ts = tss.back();

    return p;
}

} // namespace Omega
