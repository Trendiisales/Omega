#include "StrategyVQ2_17.hpp"
#include <cmath>
namespace Omega {

Decision StrategyVQ2_17::compute(const Tick& t, const OrderBook& ob, const MicroState& ms)
{
    Decision d;

    double A = (ms.gradient   * 0.32)
             + (ms.ofi        * 0.22)
             + (ms.imbalance  * 0.18)
             + (ms.wave       * 0.18)
             + (t.spread      * -0.1);

    double B = (ms.accel      * 0.45)
             + (ms.pressure   * 0.35)
             + (t.delta       * 0.10)
             + (ms.volatility * -0.10);

    double C = 0;
    for(int i=0;i<10;i++)
        C += (ob.bidSize[i] - ob.askSize[i]) * (i+1) * 0.0000013;

    double p = (A*0.35) + (B*0.45) + (C*0.20);

    d.score = p;
    d.side  = (p>=0?Side::Buy:Side::Sell);
    d.conf  = fabs(p);
    return d;
}

}
