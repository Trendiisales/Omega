#include "Strategy03.hpp"
#include <cmath>

namespace Omega {

Strategy03::Strategy03()
    : lastMid(0)
{}

double Strategy03::compute(const Tick& t,
                           const OrderBook& ob,
                           const MicroState& ms)
{
    double mid = 0.5*(t.bid + t.ask);
    double d = mid - lastMid;
    lastMid = mid;

    double depthWeight = 0;
    double B = ob.bidSize[1] + ob.bidSize[2];
    double A = ob.askSize[1] + ob.askSize[2];
    if(B + A > 0)
        depthWeight = (B - A) / (B + A);

    double mom = ms.v[5];

    return d*0.4 + depthWeight*0.4 + mom*0.2;
}

}
