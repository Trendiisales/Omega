#pragma once
#include <mutex>

namespace Omega {

class FIXStabilityGuard {
public:
    FIXStabilityGuard();

    void recordSpread(double);
    void recordVol(double);

    bool stable();

private:
    std::mutex lock;
    double lastSpread=0;
    double lastVol=0;
};

}
