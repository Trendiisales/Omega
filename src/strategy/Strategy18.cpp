#include "Strategy18.hpp"
#include <cmath>

namespace Omega {

Strategy18::Strategy18()
    : lastBid(0), lastAsk(0)
{}

double Strategy18::compute(const Tick& t,
                           const OrderBook& ob,
                           const MicroState& ms)
{
    double db = t.bid - lastBid;
    double da = t.ask - lastAsk;

    lastBid = t.bid;
    lastAsk = t.ask;

    double direction = (db + da) * 0.5;

    double obImb = 0;
    double B = ob.bidSize[0] + ob.bidSize[1] + ob.bidSize[2];
    double A = ob.askSize[0] + ob.askSize[1] + ob.askSize[2];
    if(B + A > 0)
        obImb = (B - A) / (B + A);

    double micro = (ms.v[32] + ms.v[33] + ms.v[34]) / 3.0;

    return direction*0.4 + obImb*0.4 + micro*0.2;
}

}
