#include "StrategyVQ2_13.hpp"
#include <cmath>
namespace Omega {

Decision StrategyVQ2_13::compute(const Tick& t, const OrderBook& ob, const MicroState& ms)
{
    Decision d;

    double X = (ms.liquidity  * 0.3)
             + (ms.gradient   * 0.25)
             + (ms.volatility * -0.15)
             + (ms.ofi        * 0.2)
             + (t.delta       * 0.1);

    double Y = (ms.accel     * 0.4)
             + (ms.pressure  * 0.4)
             + (ms.imbalance * 0.2);

    double Z = 0;
    for (int i=0;i<10;i++)
        Z += (ob.bidSize[i] - ob.askSize[i]) * (i+1) * 0.0000012;

    double p = (X * 0.35) + (Y * 0.45) + (Z * 0.20);

    d.score = p;
    d.side  = (p>=0?Side::Buy:Side::Sell);
    d.conf  = fabs(p);
    return d;
}

}
