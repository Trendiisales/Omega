#include "StrategyVQ2_14.hpp"
#include <cmath>
namespace Omega {

Decision StrategyVQ2_14::compute(const Tick& t, const OrderBook& ob, const MicroState& ms)
{
    Decision d;

    double A = (ms.gradient * 0.3)
             + (ms.wave     * 0.2)
             + (ms.volatility * -0.2)
             + (ms.ofi      * 0.2)
             + (ms.imbalance * 0.1);

    double B = (ms.accel    * 0.35)
             + (ms.pressure * 0.35)
             + (t.delta     * 0.1)
             + (t.spread    * -0.1);

    double C = 0;
    for (int i=0;i<10;i++)
        C += (ob.bidSize[i] + ob.askSize[i]) * 0.000001;

    double p = (A * 0.4) + (B * 0.4) + (C * 0.2);

    d.score = p;
    d.side  = (p>=0?Side::Buy:Side::Sell);
    d.conf  = fabs(p);
    return d;
}

}
