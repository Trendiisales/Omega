#pragma once
#include <deque>
#include <cstdint>
#include <mutex>

namespace Omega {

struct LiquidityShock {
    double shock = 0.0;
    double depthImpact = 0.0;
    double bidChange = 0.0;
    double askChange = 0.0;
    uint64_t ts = 0;
};

class LiquidityShockDetector {
public:
    LiquidityShockDetector();
    ~LiquidityShockDetector();

    void setWindow(size_t n);
    void setThreshold(double t);
    void add(double bidDepth,
             double askDepth,
             uint64_t ts);

    LiquidityShock compute() const;
    bool isShocked() const;
    void reset();

private:
    mutable std::mutex lock;
    size_t window;
    double threshold;
    std::deque<double> bids;
    std::deque<double> asks;
    std::deque<uint64_t> tss;
};

} // namespace Omega
