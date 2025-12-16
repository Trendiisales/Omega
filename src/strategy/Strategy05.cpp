#include "Strategy05.hpp"
#include <cmath>

namespace Omega {

Strategy05::Strategy05()
    : prevBid(0), prevAsk(0)
{}

double Strategy05::compute(const Tick& t,
                           const OrderBook& ob,
                           const MicroState& ms)
{
    double db = t.bid - prevBid;
    double da = t.ask - prevAsk;

    prevBid = t.bid;
    prevAsk = t.ask;

    double spreadExp = t.spread - (0.5 * (ob.askPrice[0] - ob.bidPrice[0]));

    double m = (ms.v[10] + ms.v[11] + ms.v[12]) / 3.0;

    return db*0.35 + da*0.35 + spreadExp*0.2 + m*0.1;
}

}
