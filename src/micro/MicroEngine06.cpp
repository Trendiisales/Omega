#include "MicroEngine06.hpp"
#include <cmath>

namespace Omega {

MicroEngine06::MicroEngine06()
    : deltaAccel(0), lastDelta(0), lastAccel(0)
{}

void MicroEngine06::update(const Tick& t, const OrderBook& ob)
{
    double d = t.delta - lastDelta;
    lastDelta = t.delta;

    double acc = d - lastAccel;
    lastAccel = d;

    deltaAccel = 0.9*deltaAccel + 0.1*acc;
}

void MicroEngine06::compute(MicroState& ms)
{
    ms.v[5] = deltaAccel;
}

}
