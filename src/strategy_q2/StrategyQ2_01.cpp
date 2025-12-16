#include "StrategyQ2_01.hpp"
#include <cmath>

namespace Omega {

StrategyQ2_01::StrategyQ2_01()
    : ema(0)
{}

double StrategyQ2_01::compute(const Tick& t,
                              const OrderBook& ob,
                              const MicroState& ms,
                              const double* base)
{
    double x = 0.5*(t.bid + t.ask);
    ema = 0.9*ema + 0.1*x;

    double dev = x - ema;

    double obImb = 0;
    double B = ob.bidSize[0] + ob.bidSize[1];
    double A = ob.askSize[0] + ob.askSize[1];
    if(B + A > 0)
        obImb = (B - A) / (B + A);

    double ultra = base[0] * 0.5 + base[1] * 0.25 + base[2] * 0.25;

    return dev*0.45 + obImb*0.35 + ms.v[0]*0.10 + ultra*0.10;
}

}
