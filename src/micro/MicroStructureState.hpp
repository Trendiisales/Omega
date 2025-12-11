#pragma once
#include <cstdint>

namespace Omega {

struct MicroStructureState {
    int64_t ts = 0;
    double mid = 0;
    double ofi = 0;
    double vpin = 0;
    double imbalance = 0;
    double shock = 0;
    double volBurst = 0;
    double flow = 0;
    int regime = 0;
    double depthRatio = 0;
};

}
