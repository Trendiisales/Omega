#include "Strategy02.hpp"
#include <cmath>

namespace Omega {

Strategy02::Strategy02()
    : lastDelta(0)
{}

double Strategy02::compute(const Tick& t,
                           const OrderBook& ob,
                           const MicroState& ms)
{
    double v = (t.delta - lastDelta);
    lastDelta = t.delta;

    double skew = 0;
    double b = ob.bidSize[0];
    double a = ob.askSize[0];
    if(b + a > 0)
        skew = (b - a) / (b + a);

    double m = ms.v[2] * 0.5 + ms.v[3] * 0.5;

    return v*0.5 + skew*0.3 + m*0.2;
}

}
