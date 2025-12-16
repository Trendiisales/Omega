#include "MicroEngine09.hpp"
#include <cmath>

namespace Omega {

MicroEngine09::MicroEngine09()
    : spreadAccel(0), lastSpread(0), lastAccel(0)
{}

void MicroEngine09::update(const Tick& t, const OrderBook& ob)
{
    double d = t.spread - lastSpread;
    lastSpread = t.spread;

    double acc = d - lastAccel;
    lastAccel = d;

    spreadAccel = 0.92*spreadAccel + 0.08*acc;
}

void MicroEngine09::compute(MicroState& ms)
{
    ms.v[8] = spreadAccel;
}

}
