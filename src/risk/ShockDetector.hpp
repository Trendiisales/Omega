#pragma once
#include "../market/Tick.hpp"

namespace Omega {

struct ShockState {
    bool   inShock     = false;
    double lastPrice   = 0.0;
    double thresholdBp = 50.0;
};

class ShockDetector {
public:
    ShockDetector();

    void setThresholdBp(double bp);
    bool update(const Tick& t);

    bool isShocked() const;

private:
    ShockState st;
};

} // namespace Omega
