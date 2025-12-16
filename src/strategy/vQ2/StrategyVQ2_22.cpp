#include "StrategyVQ2_22.hpp"
#include <cmath>
namespace Omega {

Decision StrategyVQ2_22::compute(const Tick& t, const OrderBook& ob, const MicroState& ms)
{
    Decision d;

    double B1 = (ms.gradient   * 0.33)
              + (ms.wave       * 0.22)
              + (ms.ofi        * 0.20)
              + (ms.volatility * -0.15)
              + (t.delta       * 0.10);

    double B2 = (ms.accel     * 0.40)
              + (ms.pressure  * 0.40)
              + (ms.imbalance * 0.20);

    double B3 = 0;
    for(int i=0;i<10;i++)
        B3 += (ob.bidSize[i] + ob.askSize[i]) * (i+1) * 0.000001;

    double p = (B1*0.35)+(B2*0.45)+(B3*0.20);

    d.score = p;
    d.side  = (p>=0?Side::Buy:Side::Sell);
    d.conf  = fabs(p);
    return d;
}

}
