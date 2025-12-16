#include "Strategy29.hpp"
#include <cmath>

namespace Omega {

Strategy29::Strategy29()
    : prevVol(0)
{}

double Strategy29::compute(const Tick& t,
                           const OrderBook& ob,
                           const MicroState& ms)
{
    double vol = t.buyVol + t.sellVol;
    double dv  = vol - prevVol;
    prevVol = vol;

    double depthShock = 0;
    double b = ob.bidSize[0] + ob.bidSize[2] + ob.bidSize[4] + ob.bidSize[6];
    double a = ob.askSize[0] + ob.askSize[2] + ob.askSize[4] + ob.askSize[6];
    if(b + a > 0)
        depthShock = (b - a) / (b + a);

    double micro = ms.v[47];

    return dv*0.4 + depthShock*0.4 + micro*0.2;
}

}
