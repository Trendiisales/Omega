#pragma once
#include <cstdint>
#include <deque>
#include <mutex>

namespace Omega {

class FIXSpikeGuard {
public:
    FIXSpikeGuard();
    ~FIXSpikeGuard();

    void setWindow(size_t n);
    void setThreshold(double pct);
    void addValue(double v);

    bool spikeDetected() const;
    double lastSpikeMagnitude() const;
    double average() const;
    
    void reset();

private:
    mutable std::mutex lock;
    size_t window;
    double threshold;
    std::deque<double> history;
    double lastSpike;
};

} // namespace Omega
