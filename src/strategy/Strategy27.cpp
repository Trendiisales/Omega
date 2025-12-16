#include "Strategy27.hpp"
#include <cmath>

namespace Omega {

Strategy27::Strategy27()
    : lastDelta(0)
{}

double Strategy27::compute(const Tick& t,
                           const OrderBook& ob,
                           const MicroState& ms)
{
    double d = t.delta - lastDelta;
    lastDelta = t.delta;

    double lvl = 0;
    double B = ob.bidSize[1] + ob.bidSize[2];
    double A = ob.askSize[1] + ob.askSize[2];
    if(B + A > 0)
        lvl = (B - A) / (B + A);

    double micro = (ms.v[44] + ms.v[45]) * 0.5;

    return d*0.4 + lvl*0.4 + micro*0.2;
}

}
