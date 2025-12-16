#include "Strategy21.hpp"
#include <cmath>

namespace Omega {

Strategy21::Strategy21()
    : prev(0)
{}

double Strategy21::compute(const Tick& t,
                           const OrderBook& ob,
                           const MicroState& ms)
{
    double m = 0.5*(t.bid + t.ask);
    double change = m - prev;
    prev = m;

    double orderflow = t.buyVol - t.sellVol;

    double topTilt = 0;
    double b = ob.bidSize[0];
    double a = ob.askSize[0];
    if(b + a > 0)
        topTilt = (b - a) / (b + a);

    double micro = ms.v[38];

    return change*0.35 + orderflow*0.35 + topTilt*0.2 + micro*0.1;
}

}
