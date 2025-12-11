#include "Strategy01.hpp"
#include <cmath>

namespace Omega {

Strategy01::Strategy01()
    : prev(0)
{}

double Strategy01::compute(const Tick& t,
                           const OrderBook& ob,
                           const MicroState& ms)
{
    double mid = 0.5*(t.bid + t.ask);
    double m = ms.v[0] - ms.v[1];

    double d = mid - prev;
    prev = mid;

    double imb = 0;
    double denom = (ob.bidSize[0] + ob.askSize[0]);
    if(denom > 0)
        imb = (ob.bidSize[0] - ob.askSize[0]) / denom;

    return d*0.4 + imb*0.3 + m*0.3;
}

}
