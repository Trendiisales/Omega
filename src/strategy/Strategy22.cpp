#include "Strategy22.hpp"
#include <cmath>

namespace Omega {

Strategy22::Strategy22()
    : lastMid(0), drift(0)
{}

double Strategy22::compute(const Tick& t,
                           const OrderBook& ob,
                           const MicroState& ms)
{
    double mid = 0.5*(t.bid + t.ask);

    double d = mid - lastMid;
    lastMid = mid;

    drift = 0.9*drift + 0.1*d;

    double lvlSkew = 0;
    double B = ob.bidSize[2] + ob.bidSize[3];
    double A = ob.askSize[2] + ob.askSize[3];
    if(B + A > 0)
        lvlSkew = (B - A) / (B + A);

    double micro = ms.v[39];

    return drift*0.4 + d*0.3 + lvlSkew*0.2 + micro*0.1;
}

}
