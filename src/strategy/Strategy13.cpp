#include "Strategy13.hpp"
#include <cmath>

namespace Omega {

Strategy13::Strategy13()
    : emaMid(0)
{}

double Strategy13::compute(const Tick& t,
                           const OrderBook& ob,
                           const MicroState& ms)
{
    double mid = 0.5*(t.bid + t.ask);
    emaMid = 0.9*emaMid + 0.1*mid;

    double deviation = mid - emaMid;

    double l1 = ob.bidSize[0] + ob.askSize[0];
    double l2 = ob.bidSize[1] + ob.askSize[1];
    double volRatio = (l2 > 0 ? l1 / l2 : 0);

    double micro = ms.v[26];

    return deviation*0.4 + volRatio*0.4 + micro*0.2;
}

}
