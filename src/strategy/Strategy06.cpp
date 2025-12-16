#include "Strategy06.hpp"
#include <cmath>

namespace Omega {

Strategy06::Strategy06()
    : lastMid(0), lastVol(0)
{}

double Strategy06::compute(const Tick& t,
                           const OrderBook& ob,
                           const MicroState& ms)
{
    double mid = 0.5*(t.bid + t.ask);
    double dm = mid - lastMid;
    lastMid = mid;

    double vol = t.buyVol + t.sellVol;
    double dv = vol - lastVol;
    lastVol = vol;

    double skew = 0;
    double b = ob.bidSize[0] + ob.bidSize[1];
    double a = ob.askSize[0] + ob.askSize[1];
    if(b + a > 0)
        skew = (b - a) / (b + a);

    double microMom = ms.v[14];

    return dm*0.3 + dv*0.3 + skew*0.3 + microMom*0.1;
}

}
