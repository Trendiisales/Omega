#include "StrategyQ2_06.hpp"
#include <cmath>

namespace Omega {

StrategyQ2_06::StrategyQ2_06()
    : buySell(0)
{}

double StrategyQ2_06::compute(const Tick& t,
                              const OrderBook& ob,
                              const MicroState& ms,
                              const double* base)
{
    double v = t.buyVol + t.sellVol;
    if(v > 0)
        buySell = (t.buyVol - t.sellVol) / v;
    else
        buySell = 0;

    double top = 0;
    double b = ob.bidSize[0];
    double a = ob.askSize[0];
    if(b + a > 0)
        top = (b - a) / (b + a);

    double fuse = base[14]*0.5 + base[15]*0.5;

    return buySell*0.45 + top*0.30 + ms.v[7]*0.15 + fuse*0.10;
}

}
