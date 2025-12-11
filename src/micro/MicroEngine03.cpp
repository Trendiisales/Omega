#include "MicroEngine03.hpp"
#include <cmath>

namespace Omega {

MicroEngine03::MicroEngine03()
    : volEMA(0), lastVol(0)
{}

void MicroEngine03::update(const Tick& t, const OrderBook& ob)
{
    double v = t.buyVol + t.sellVol;
    double dv = v - lastVol;
    lastVol = v;

    volEMA = 0.9*volEMA + 0.1*dv;
}

void MicroEngine03::compute(MicroState& ms)
{
    ms.v[2] = volEMA;
}

}
