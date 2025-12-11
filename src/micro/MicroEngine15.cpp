#include "MicroEngine15.hpp"
#include <cmath>

namespace Omega {

MicroEngine15::MicroEngine15()
    : spreadTrend(0), lastSpread(0)
{}

void MicroEngine15::update(const Tick& t, const OrderBook& ob)
{
    double d = t.spread - lastSpread;
    lastSpread = t.spread;

    spreadTrend = 0.85*spreadTrend + 0.15*d;
}

void MicroEngine15::compute(MicroState& ms)
{
    ms.v[14] = spreadTrend;
}

}
