#include "StrategyQ2_05.hpp"
#include <cmath>

namespace Omega {

StrategyQ2_05::StrategyQ2_05()
    : lastMid(0), accel(0)
{}

double StrategyQ2_05::compute(const Tick& t,
                              const OrderBook& ob,
                              const MicroState& ms,
                              const double* base)
{
    double mid = 0.5*(t.bid + t.ask);
    double d = mid - lastMid;
    lastMid = mid;

    accel = 0.85*accel + 0.15*d;

    double depth = 0;
    double B = ob.bidSize[2] + ob.bidSize[3] + ob.bidSize[4];
    double A = ob.askSize[2] + ob.askSize[3] + ob.askSize[4];
    if(B + A > 0)
        depth = (B - A) / (B + A);

    double fuse = (base[11] + base[12] + base[13]) / 3.0;

    return accel*0.40 + d*0.20 + depth*0.25 + ms.v[5]*0.10 + fuse*0.05;
}

}
