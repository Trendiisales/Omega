#include "StrategyVQ2_16.hpp"
#include <cmath>
namespace Omega {

Decision StrategyVQ2_16::compute(const Tick& t, const OrderBook& ob, const MicroState& ms)
{
    Decision d;

    double Z1 = (ms.gradient  * 0.3)
              + (ms.wave      * 0.2)
              + (ms.volatility * -0.2)
              + (ms.ofi       * 0.2)
              + (ms.imbalance * 0.1);

    double Z2 = (ms.accel    * 0.35)
              + (ms.pressure * 0.35)
              + (t.delta     * 0.1)
              + (t.spread    * -0.1);

    double Z3 = 0;
    for (int i=0;i<10;i++)
        Z3 += (ob.bidSize[i] + ob.askSize[i]) * 0.0000011;

    double p = (Z1 * 0.4) + (Z2 * 0.4) + (Z3 * 0.2);

    d.score = p;
    d.side = (p>=0?Side::Buy:Side::Sell);
    d.conf = fabs(p);
    return d;
}

}
