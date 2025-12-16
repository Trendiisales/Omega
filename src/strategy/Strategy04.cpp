#include "Strategy04.hpp"
#include <cmath>

namespace Omega {

Strategy04::Strategy04()
    : last(0)
{}

double Strategy04::compute(const Tick& t,
                           const OrderBook& ob,
                           const MicroState& ms)
{
    double mid = 0.5*(t.bid + t.ask);
    double chg = mid - last;
    last = mid;

    double pressure = 0;
    double B = ob.bidSize[0] + ob.bidSize[1] + ob.bidSize[2];
    double A = ob.askSize[0] + ob.askSize[1] + ob.askSize[2];
    if(B + A > 0)
        pressure = (B - A) / (B + A);

    double volSig = ms.v[6] - ms.v[7];

    return chg*0.4 + pressure*0.4 + volSig*0.2;
}

}
