#include "Strategy08.hpp"
#include <cmath>

namespace Omega {

Strategy08::Strategy08()
    : avgVol(0)
{}

double Strategy08::compute(const Tick& t,
                           const OrderBook& ob,
                           const MicroState& ms)
{
    double vol = t.buyVol + t.sellVol;
    avgVol = 0.9*avgVol + 0.1*vol;

    double burst = vol - avgVol;

    double imbalance = 0;
    double b = ob.bidSize[0];
    double a = ob.askSize[0];
    if(b + a > 0)
        imbalance = (b - a) / (b + a);

    double micro = (ms.v[18] + ms.v[19])*0.5;

    return burst*0.4 + imbalance*0.4 + micro*0.2;
}

}
