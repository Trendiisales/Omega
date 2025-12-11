#include "Strategy12.hpp"
#include <cmath>

namespace Omega {

Strategy12::Strategy12()
    : lastDelta(0), accel(0)
{}

double Strategy12::compute(const Tick& t,
                           const OrderBook& ob,
                           const MicroState& ms)
{
    double d = t.delta - lastDelta;
    lastDelta = t.delta;

    accel = 0.8*accel + 0.2*d;

    double lvlSkew = 0;
    double B = ob.bidSize[0] + ob.bidSize[1];
    double A = ob.askSize[0] + ob.askSize[1];
    if(B + A > 0)
        lvlSkew = (B - A) / (B + A);

    double micro = ms.v[25];

    return accel*0.4 + d*0.3 + lvlSkew*0.2 + micro*0.1;
}

}
