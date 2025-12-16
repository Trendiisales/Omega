#include "StrategyQ2_02.hpp"
#include <cmath>

namespace Omega {

StrategyQ2_02::StrategyQ2_02()
    : last(0), trend(0)
{}

double StrategyQ2_02::compute(const Tick& t,
                              const OrderBook& ob,
                              const MicroState& ms,
                              const double* base)
{
    double mid = 0.5*(t.bid + t.ask);

    double d = mid - last;
    last = mid;

    trend = 0.92*trend + 0.08*d;

    double ob2 = 0;
    double B = ob.bidSize[2] + ob.bidSize[3] + ob.bidSize[4];
    double A = ob.askSize[2] + ob.askSize[3] + ob.askSize[4];
    if(B + A > 0)
        ob2 = (B - A) / (B + A);

    double fuse = (base[3] + base[4] + base[5]) / 3.0;

    return trend*0.4 + d*0.2 + ob2*0.25 + ms.v[4]*0.10 + fuse*0.05;
}

}
