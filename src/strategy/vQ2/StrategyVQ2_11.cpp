#include "StrategyVQ2_11.hpp"
#include <cmath>
namespace Omega {

Decision StrategyVQ2_11::compute(const Tick& t, const OrderBook& ob, const MicroState& ms)
{
    Decision d;

    double F = (ms.impact    * 0.3)
             + (ms.gradient  * 0.3)
             + (ms.wave      * 0.2)
             + (ms.ofi       * 0.1)
             + (ms.volatility * -0.1);

    double G = (ms.accel     * 0.4)
             + (ms.pressure  * 0.4)
             + (ms.imbalance * 0.2);

    double H = 0;
    for (int i=0;i<10;i++)
        H += (ob.bidSize[i] - ob.askSize[i]) * (i+1) * 0.000001;

    double p = (F * 0.45) + (G * 0.35) + (H * 0.20);

    d.score = p;
    d.side  = (p>=0?Side::Buy:Side::Sell);
    d.conf  = fabs(p);
    return d;
}

}
