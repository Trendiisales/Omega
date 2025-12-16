#include "Strategy15.hpp"
#include <cmath>

namespace Omega {

Strategy15::Strategy15()
    : drift(0), lastMid(0)
{}

double Strategy15::compute(const Tick& t,
                           const OrderBook& ob,
                           const MicroState& ms)
{
    double mid = 0.5*(t.bid + t.ask);
    double d = mid - lastMid;
    lastMid = mid;

    drift = 0.95*drift + 0.05*d;

    double lvl = 0;
    double b = ob.bidSize[1] + ob.bidSize[2];
    double a = ob.askSize[1] + ob.askSize[2];
    if(b + a > 0)
        lvl = (b - a) / (b + a);

    double micro = ms.v[29];

    return drift*0.4 + d*0.3 + lvl*0.2 + micro*0.1;
}

}
