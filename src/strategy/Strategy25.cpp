#include "Strategy25.hpp"
#include <cmath>

namespace Omega {

Strategy25::Strategy25()
    : emaMid(0), emaVol(0)
{}

double Strategy25::compute(const Tick& t,
                           const OrderBook& ob,
                           const MicroState& ms)
{
    double mid = 0.5*(t.bid + t.ask);
    double vol = t.buyVol + t.sellVol;

    emaMid = 0.9*emaMid + 0.1*mid;
    emaVol = 0.9*emaVol + 0.1*vol;

    double devMid = mid - emaMid;
    double devVol = vol - emaVol;

    double skew = 0;
    double B = ob.bidSize[0] + ob.bidSize[1];
    double A = ob.askSize[0] + ob.askSize[1];
    if(B + A > 0)
        skew = (B - A) / (B + A);

    double micro = ms.v[42];

    return devMid*0.35 + devVol*0.35 + skew*0.2 + micro*0.1;
}

}
