#include "StrategyVQ2_26.hpp"
#include <cmath>
namespace Omega {

Decision StrategyVQ2_26::compute(const Tick& t, const OrderBook& ob, const MicroState& ms)
{
    Decision d;

    double L1 = (ms.gradient  * 0.35)
              + (ms.wave     * 0.25)
              + (ms.ofi      * 0.20)
              + (t.spread    * -0.10)
              + (ms.volatility * -0.10);

    double L2 = (ms.accel     * 0.40)
              + (ms.pressure  * 0.40)
              + (ms.imbalance * 0.20);

    double L3 = 0;
    for(int i=0;i<10;i++)
        L3 += (ob.bidSize[i] + ob.askSize[i]) * (i+1) * 0.0000014;

    double p = (L1*0.4)+(L2*0.4)+(L3*0.2);

    d.score = p;
    d.side = (p>=0?Side::Buy:Side::Sell);
    d.conf = fabs(p);
    return d;
}

}
