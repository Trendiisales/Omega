#include "MicroEngine08.hpp"
#include <cmath>

namespace Omega {

MicroEngine08::MicroEngine08()
    : volShock(0), volAvg(0)
{}

void MicroEngine08::update(const Tick& t, const OrderBook& ob)
{
    double v = t.buyVol + t.sellVol;
    volAvg = 0.9*volAvg + 0.1*v;

    volShock = v - volAvg;
}

void MicroEngine08::compute(MicroState& ms)
{
    ms.v[7] = volShock;
}

}
