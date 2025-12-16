#include "Strategy14.hpp"
#include <cmath>

namespace Omega {

Strategy14::Strategy14()
    : lastBuy(0), lastSell(0)
{}

double Strategy14::compute(const Tick& t,
                           const OrderBook& ob,
                           const MicroState& ms)
{
    double buyFlow  = t.buyVol  - lastBuy;
    double sellFlow = t.sellVol - lastSell;

    lastBuy  = t.buyVol;
    lastSell = t.sellVol;

    double flowImb = (buyFlow - sellFlow);

    double d2 = 0;
    double B = ob.bidSize[0] + ob.bidSize[1] + ob.bidSize[2];
    double A = ob.askSize[0] + ob.askSize[1] + ob.askSize[2];
    if(B + A > 0)
        d2 = (B - A) / (B + A);

    double micro = ms.v[27] + ms.v[28];

    return flowImb*0.5 + d2*0.3 + micro*0.2;
}

}
