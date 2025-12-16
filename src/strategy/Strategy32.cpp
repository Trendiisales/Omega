#include "Strategy32.hpp"
#include <cmath>

namespace Omega {

Strategy32::Strategy32()
    : smooth(0), lastTradePrice(0)
{}

double Strategy32::compute(const Tick& t,
                           const OrderBook& ob,
                           const MicroState& ms)
{
    double tradePx = (t.bid + t.ask) * 0.5;
    double diff = tradePx - lastTradePrice;
    lastTradePrice = tradePx;

    smooth = 0.93*smooth + 0.07*diff;

    double topDepth = 0;
    double b = ob.bidSize[0];
    double a = ob.askSize[0];
    if(b + a > 0)
        topDepth = (b - a) / (b + a);

    double micro = (ms.v[52] + ms.v[53] + ms.v[54] + ms.v[55]) / 4.0;

    return smooth*0.45 + diff*0.25 + topDepth*0.20 + micro*0.10;
}

}
