#include "Strategy31.hpp"
#include <cmath>

namespace Omega {

Strategy31::Strategy31()
    : emaImpulse(0), lastMid(0), lastDelta(0)
{}

double Strategy31::compute(const Tick& t,
                           const OrderBook& ob,
                           const MicroState& ms)
{
    double mid = 0.5*(t.bid + t.ask);
    double dMid = mid - lastMid;
    lastMid = mid;

    double dDel = t.delta - lastDelta;
    lastDelta = t.delta;

    double impulse = dMid + dDel;
    emaImpulse = 0.92*emaImpulse + 0.08*impulse;

    double bookSlope = 0;
    double B = ob.bidSize[0] + ob.bidSize[2] + ob.bidSize[4] + ob.bidSize[6] + ob.bidSize[8];
    double A = ob.askSize[0] + ob.askSize[2] + ob.askSize[4] + ob.askSize[6] + ob.askSize[8];
    if(B + A > 0)
        bookSlope = (B - A) / (B + A);

    double micro = (ms.v[49] + ms.v[50] + ms.v[51]) / 3.0;

    return emaImpulse*0.45 + bookSlope*0.35 + micro*0.20;
}

}
