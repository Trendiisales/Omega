#pragma once
#include <mutex>
#include <deque>

namespace Omega {

class FIXLatencyMonitor {
public:
    FIXLatencyMonitor();

    void recordSend(long ts);
    void recordRecv(long ts);

    double avgLatencyMs();
    double p95();
    double p99();

private:
    std::mutex lock;
    std::deque<long> samples;
    std::deque<long> sendQueue;
};

}
