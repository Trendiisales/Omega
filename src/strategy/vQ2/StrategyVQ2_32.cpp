#include "StrategyVQ2_32.hpp"
#include <cmath>
namespace Omega {

Decision StrategyVQ2_32::compute(const Tick& t, const OrderBook& ob, const MicroState& ms)
{
    Decision d;

    double Z1 = (ms.gradient   * 0.33)
              + (ms.wave       * 0.22)
              + (ms.ofi        * 0.18)
              + (ms.volatility * -0.15)
              + (t.delta       * 0.12);

    double Z2 = (ms.accel     * 0.40)
              + (ms.pressure  * 0.40)
              + (ms.imbalance * 0.20);

    double Z3 = 0;
    for(int i=0;i<10;i++)
        Z3 += (ob.bidSize[i] - ob.askSize[i]) * (i+1) * 0.0000012;

    double p = (Z1*0.40)+(Z2*0.40)+(Z3*0.20);

    d.score = p;
    d.side = (p>=0?Side::Buy:Side::Sell);
    d.conf = fabs(p);

    return d;
}

}
