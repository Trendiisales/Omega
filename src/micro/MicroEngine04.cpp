#include "MicroEngine04.hpp"
#include <cmath>

namespace Omega {

MicroEngine04::MicroEngine04()
    : spreadEMA(0)
{}

void MicroEngine04::update(const Tick& t, const OrderBook& ob)
{
    spreadEMA = 0.85*spreadEMA + 0.15*t.spread;
}

void MicroEngine04::compute(MicroState& ms)
{
    ms.v[3] = spreadEMA;
}

}
