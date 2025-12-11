#include "StrategyQ2_03.hpp"
#include <cmath>

namespace Omega {

StrategyQ2_03::StrategyQ2_03()
    : drift(0), lastMid(0)
{}

double StrategyQ2_03::compute(const Tick& t,
                              const OrderBook& ob,
                              const MicroState& ms,
                              const double* base)
{
    double mid = 0.5*(t.bid + t.ask);

    double d = mid - lastMid;
    lastMid = mid;

    drift = 0.9*drift + 0.1*d;

    double obTop = 0;
    double b = ob.bidSize[0];
    double a = ob.askSize[0];
    if(b + a > 0)
        obTop = (b - a) / (b + a);

    double blend = base[6]*0.5 + base[7]*0.3 + base[8]*0.2;

    return drift*0.35 + d*0.25 + obTop*0.20 + ms.v[2]*0.10 + blend*0.10;
}

}
