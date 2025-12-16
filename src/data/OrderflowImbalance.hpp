#pragma once
#include <cstdint>
#include <deque>
#include <mutex>

namespace Omega {

struct OFIResult {
    double ofi = 0.0;
    double ratio = 0.0;
    double cumulative = 0.0;
    uint64_t ts = 0;
};

class OrderflowImbalance {
public:
    OrderflowImbalance();
    ~OrderflowImbalance();

    void setWindow(size_t n);

    void add(double bidVol,
             double askVol,
             uint64_t ts);

    OFIResult compute() const;
    void reset();

private:
    mutable std::mutex lock;
    size_t window;
    std::deque<double> bids;
    std::deque<double> asks;
    std::deque<uint64_t> tss;
    double cumOfi;
};

} // namespace Omega
