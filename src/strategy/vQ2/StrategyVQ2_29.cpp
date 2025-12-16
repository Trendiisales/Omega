#include "StrategyVQ2_29.hpp"
#include <cmath>
namespace Omega {

Decision StrategyVQ2_29::compute(const Tick& t, const OrderBook& ob, const MicroState& ms)
{
    Decision d;

    double G1 = (ms.gradient  * 0.32)
              + (ms.ofi       * 0.22)
              + (ms.imbalance * 0.18)
              + (ms.wave      * 0.18)
              + (ms.volatility * -0.10);

    double G2 = (ms.accel     * 0.45)
              + (ms.pressure  * 0.35)
              + (t.delta      * 0.10)
              + (ms.liquidity * 0.10);

    double G3 = 0;
    for(int i=0;i<10;i++)
        G3 += (ob.bidSize[i] - ob.askSize[i]) * (i+1) * 0.00000115;

    double p = (G1*0.40)+(G2*0.40)+(G3*0.20);

    d.score = p;
    d.side = (p>=0?Side::Buy:Side::Sell);
    d.conf = fabs(p);

    return d;
}

}
