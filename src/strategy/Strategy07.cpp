#include "Strategy07.hpp"
#include <cmath>

namespace Omega {

Strategy07::Strategy07()
    : lastMid(0), lastDelta(0)
{}

double Strategy07::compute(const Tick& t,
                           const OrderBook& ob,
                           const MicroState& ms)
{
    double mid = 0.5*(t.bid + t.ask);
    double dm  = mid - lastMid;
    lastMid = mid;

    double dd  = t.delta - lastDelta;
    lastDelta = t.delta;

    double depth2 = 0;
    double B = ob.bidSize[0] + ob.bidSize[1] + ob.bidSize[2] + ob.bidSize[3];
    double A = ob.askSize[0] + ob.askSize[1] + ob.askSize[2] + ob.askSize[3];
    if(B + A > 0)
        depth2 = (B - A) / (B + A);

    double micro = (ms.v[15] - ms.v[16]);

    return dm*0.35 + dd*0.25 + depth2*0.25 + micro*0.15;
}

}
