#include "Strategy17.hpp"
#include <cmath>

namespace Omega {

Strategy17::Strategy17()
    : volAvg(0)
{}

double Strategy17::compute(const Tick& t,
                           const OrderBook& ob,
                           const MicroState& ms)
{
    double v = t.buyVol + t.sellVol;
    volAvg = 0.95*volAvg + 0.05*v;

    double volShock = v - volAvg;

    double depthTilt = 0;
    double B = ob.bidSize[2] + ob.bidSize[3] + ob.bidSize[4];
    double A = ob.askSize[2] + ob.askSize[3] + ob.askSize[4];
    if(B + A > 0)
        depthTilt = (B - A) / (B + A);

    double micro = ms.v[31];

    return volShock*0.4 + depthTilt*0.4 + micro*0.2;
}

}
