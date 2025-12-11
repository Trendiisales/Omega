#include "Strategy11.hpp"
#include <cmath>

namespace Omega {

Strategy11::Strategy11()
    : avgSpread(0)
{}

double Strategy11::compute(const Tick& t,
                           const OrderBook& ob,
                           const MicroState& ms)
{
    avgSpread = 0.9*avgSpread + 0.1*t.spread;

    double spreadDev = t.spread - avgSpread;

    double sq = 0;
    double b = ob.bidSize[0];
    double a = ob.askSize[0];
    if(b + a > 0)
        sq = (b - a) / (b + a);

    double micro = (ms.v[23] - ms.v[24]);

    return spreadDev*0.45 + sq*0.35 + micro*0.2;
}

}
