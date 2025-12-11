#include "Strategy19.hpp"
#include <cmath>

namespace Omega {

Strategy19::Strategy19()
    : emaMid(0), emaSpread(0)
{}

double Strategy19::compute(const Tick& t,
                           const OrderBook& ob,
                           const MicroState& ms)
{
    double mid = 0.5*(t.bid + t.ask);
    emaMid    = 0.92*emaMid    + 0.08*mid;
    emaSpread = 0.92*emaSpread + 0.08*t.spread;

    double devMid    = mid - emaMid;
    double devSpread = t.spread - emaSpread;

    double bookTilt = 0;
    double B = ob.bidSize[0] + ob.bidSize[2] + ob.bidSize[4] + ob.bidSize[6];
    double A = ob.askSize[0] + ob.askSize[2] + ob.askSize[4] + ob.askSize[6];
    if(B + A > 0)
        bookTilt = (B - A) / (B + A);

    double micro = ms.v[35];

    return devMid*0.4 + devSpread*0.3 + bookTilt*0.2 + micro*0.1;
}

}
