#include "StrategyQ2_04.hpp"
#include <cmath>

namespace Omega {

StrategyQ2_04::StrategyQ2_04()
    : emaSpread(0)
{}

double StrategyQ2_04::compute(const Tick& t,
                              const OrderBook& ob,
                              const MicroState& ms,
                              const double* base)
{
    emaSpread = 0.92*emaSpread + 0.08*t.spread;
    double dev = t.spread - emaSpread;

    double obTilt = 0;
    double B = ob.bidSize[0] + ob.bidSize[1];
    double A = ob.askSize[0] + ob.askSize[1];
    if(B + A > 0)
        obTilt = (B - A) / (B + A);

    double fuse = (base[9] + base[10]) * 0.5;

    return dev*0.45 + obTilt*0.35 + ms.v[3]*0.10 + fuse*0.10;
}

}
