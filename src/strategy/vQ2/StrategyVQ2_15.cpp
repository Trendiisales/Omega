#include "StrategyVQ2_15.hpp"
#include <cmath>
namespace Omega {

Decision StrategyVQ2_15::compute(const Tick& t, const OrderBook& ob, const MicroState& ms)
{
    Decision d;

    double G1 = (ms.gradient  * 0.35)
              + (ms.imbalance * 0.25)
              + (ms.ofi       * 0.15)
              + (ms.volatility * -0.1)
              + (ms.liquidity * 0.05)
              + (t.delta      * 0.10);

    double G2 = (ms.accel    * 0.45)
              + (ms.pressure * 0.35)
              + (ms.wave     * 0.20);

    double G3 = 0;
    for (int i=0;i<10;i++)
        G3 += (ob.bidSize[i] - ob.askSize[i]) * 0.0000011;

    double p = (G1 * 0.4) + (G2 * 0.4) + (G3 * 0.2);

    d.score = p;
    d.side  = (p>=0?Side::Buy:Side::Sell);
    d.conf  = fabs(p);
    return d;
}

}
