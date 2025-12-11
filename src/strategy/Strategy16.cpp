#include "Strategy16.hpp"
#include <cmath>

namespace Omega {

Strategy16::Strategy16()
    : emaDelta(0)
{}

double Strategy16::compute(const Tick& t,
                           const OrderBook& ob,
                           const MicroState& ms)
{
    emaDelta = 0.9*emaDelta + 0.1*t.delta;

    double dev = t.delta - emaDelta;

    double lvlSkew = 0;
    double b = ob.bidSize[0] + ob.bidSize[3];
    double a = ob.askSize[0] + ob.askSize[3];
    if(b + a > 0)
        lvlSkew = (b - a) / (b + a);

    double micro = ms.v[30];

    return dev*0.45 + lvlSkew*0.35 + micro*0.2;
}

}
