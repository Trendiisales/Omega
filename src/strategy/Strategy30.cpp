#include "Strategy30.hpp"
#include <cmath>

namespace Omega {

Strategy30::Strategy30()
    : drift(0), lastMid(0)
{}

double Strategy30::compute(const Tick& t,
                           const OrderBook& ob,
                           const MicroState& ms)
{
    double mid = 0.5*(t.bid + t.ask);

    double d = mid - lastMid;
    lastMid = mid;

    drift = 0.9*drift + 0.1*d;

    double lvlTilt = 0;
    double B = ob.bidSize[1] + ob.bidSize[2] + ob.bidSize[3];
    double A = ob.askSize[1] + ob.askSize[2] + ob.askSize[3];
    if(B + A > 0)
        lvlTilt = (B - A) / (B + A);

    double micro = ms.v[48];

    return drift*0.4 + d*0.3 + lvlTilt*0.2 + micro*0.1;
}

}
