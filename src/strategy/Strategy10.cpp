#include "Strategy10.hpp"
#include <cmath>

namespace Omega {

Strategy10::Strategy10()
    : lastMid(0), trend(0)
{}

double Strategy10::compute(const Tick& t,
                           const OrderBook& ob,
                           const MicroState& ms)
{
    double mid = 0.5*(t.bid + t.ask);
    double d = mid - lastMid;
    lastMid = mid;

    trend = 0.95*trend + 0.05*d;

    double depthTilt = 0;
    double Bb = ob.bidSize[0] + ob.bidSize[1] + ob.bidSize[2] + ob.bidSize[3];
    double Aa = ob.askSize[0] + ob.askSize[1] + ob.askSize[2] + ob.askSize[3];
    if(Bb + Aa > 0)
        depthTilt = (Bb - Aa) / (Bb + Aa);

    double micro = ms.v[22];

    return trend*0.4 + d*0.3 + depthTilt*0.2 + micro*0.1;
}

}
