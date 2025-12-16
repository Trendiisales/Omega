#include "Strategy24.hpp"
#include <cmath>

namespace Omega {

Strategy24::Strategy24()
    : lastBid(0), lastAsk(0), spreadEMA(0)
{}

double Strategy24::compute(const Tick& t,
                           const OrderBook& ob,
                           const MicroState& ms)
{
    double db = t.bid - lastBid;
    double da = t.ask - lastAsk;

    lastBid = t.bid;
    lastAsk = t.ask;

    double delta = (db + da) * 0.5;

    spreadEMA = 0.8*spreadEMA + 0.2*t.spread;

    double spreadDev = t.spread - spreadEMA;

    double obSkew = 0;
    double B = ob.bidSize[1] + ob.bidSize[2] + ob.bidSize[3];
    double A = ob.askSize[1] + ob.askSize[2] + ob.askSize[3];
    if(B + A > 0)
        obSkew = (B - A) / (B + A);

    double micro = ms.v[41];

    return delta*0.35 + spreadDev*0.35 + obSkew*0.2 + micro*0.1;
}

}
