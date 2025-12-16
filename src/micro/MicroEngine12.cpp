#include "MicroEngine12.hpp"
#include <cmath>

namespace Omega {

MicroEngine12::MicroEngine12()
    : volBalance(0)
{}

void MicroEngine12::update(const Tick& t, const OrderBook& ob)
{
    double buy  = t.buyVol;
    double sell = t.sellVol;

    if(buy + sell > 0)
        volBalance = (buy - sell) / (buy + sell);
    else
        volBalance = 0;
}

void MicroEngine12::compute(MicroState& ms)
{
    ms.v[11] = volBalance;
}

}
