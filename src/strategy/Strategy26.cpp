#include "Strategy26.hpp"
#include <cmath>

namespace Omega {

Strategy26::Strategy26()
    : trend(0), lastMid(0)
{}

double Strategy26::compute(const Tick& t,
                           const OrderBook& ob,
                           const MicroState& ms)
{
    double mid = 0.5*(t.bid + t.ask);

    double d = mid - lastMid;
    lastMid = mid;

    trend = 0.95*trend + 0.05*d;

    double depth = 0;
    double B = ob.bidSize[2] + ob.bidSize[3] + ob.bidSize[4];
    double A = ob.askSize[2] + ob.askSize[3] + ob.askSize[4];
    if(B + A > 0)
        depth = (B - A) / (B + A);

    double micro = ms.v[43];

    return trend*0.4 + d*0.3 + depth*0.2 + micro*0.1;
}

}
