#pragma once
#include <deque>
#include <cstdint>
#include <mutex>

namespace Omega {

struct VolBurst {
    double burst = 0.0;
    double sigma = 0.0;
    double zscore = 0.0;
    uint64_t ts = 0;
};

class VolatilityBurst {
public:
    VolatilityBurst();
    ~VolatilityBurst();

    void setWindow(size_t n);
    void setThreshold(double t);
    void add(double mid, uint64_t ts);

    VolBurst compute() const;
    bool isBursting() const;
    void reset();

private:
    mutable std::mutex lock;
    size_t window;
    double threshold;
    std::deque<double> mids;
    std::deque<uint64_t> tss;
};

} // namespace Omega
