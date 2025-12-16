#include "Strategy23.hpp"
#include <cmath>

namespace Omega {

Strategy23::Strategy23()
    : lastVol(0), volEMA(0)
{}

double Strategy23::compute(const Tick& t,
                           const OrderBook& ob,
                           const MicroState& ms)
{
    double vol = t.buyVol + t.sellVol;

    double dv = vol - lastVol;
    lastVol = vol;

    volEMA = 0.9*volEMA + 0.1*dv;

    double obBalance = 0;
    double B = ob.bidSize[0] + ob.bidSize[1] + ob.bidSize[2];
    double A = ob.askSize[0] + ob.askSize[1] + ob.askSize[2];
    if(B + A > 0)
        obBalance = (B - A) / (B + A);

    double micro = ms.v[40];

    return volEMA*0.4 + dv*0.3 + obBalance*0.2 + micro*0.1;
}

}
