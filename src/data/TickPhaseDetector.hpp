#pragma once
#include <cstdint>
#include <deque>
#include <mutex>

namespace Omega {

struct TickPhase {
    double impulse = 0.0;
    double direction = 0.0;
    double velocity = 0.0;
    double acceleration = 0.0;
    uint64_t ts = 0;
};

class TickPhaseDetector {
public:
    TickPhaseDetector();
    ~TickPhaseDetector();

    void setWindow(size_t n);
    void add(double mid, uint64_t ts);

    TickPhase compute() const;
    void reset();

private:
    mutable std::mutex lock;
    size_t window;
    std::deque<double> mids;
    std::deque<uint64_t> tss;
};

} // namespace Omega
