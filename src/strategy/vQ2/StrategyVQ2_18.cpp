#include "StrategyVQ2_18.hpp"
#include <cmath>
namespace Omega {

Decision StrategyVQ2_18::compute(const Tick& t, const OrderBook& ob, const MicroState& ms)
{
    Decision d;

    double X = (ms.gradient   * 0.30)
             + (ms.liquidity  * 0.20)
             + (ms.volatility * -0.15)
             + (ms.ofi        * 0.20)
             + (t.delta       * 0.10)
             + (ms.wave       * 0.05);

    double Y = (ms.accel     * 0.40)
             + (ms.pressure  * 0.40)
             + (ms.imbalance * 0.20);

    double Z = 0;
    for(int i=0;i<10;i++)
        Z += (ob.bidSize[i] + ob.askSize[i]) * (i+1) * 0.0000011;

    double p = (X*0.35)+(Y*0.45)+(Z*0.20);

    d.score = p;
    d.side  = (p>=0?Side::Buy:Side::Sell);
    d.conf  = fabs(p);

    return d;
}

}
