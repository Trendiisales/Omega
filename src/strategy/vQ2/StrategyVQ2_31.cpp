#include "StrategyVQ2_31.hpp"
#include <cmath>
namespace Omega {

Decision StrategyVQ2_31::compute(const Tick& t, const OrderBook& ob, const MicroState& ms)
{
    Decision d;

    double P1 = (ms.liquidity * 0.28)
              + (ms.gradient  * 0.32)
              + (ms.wave      * 0.20)
              + (ms.ofi       * 0.20);

    double P2 = (ms.accel     * 0.40)
              + (ms.pressure  * 0.40)
              + (ms.imbalance * 0.20);

    double P3 = 0;
    for(int i=0;i<10;i++)
        P3 += (ob.bidSize[i] - ob.askSize[i]) * (i+1) * 0.00000135;

    double p = (P1*0.40)+(P2*0.40)+(P3*0.20);

    d.score = p;
    d.side = (p>=0?Side::Buy:Side::Sell);
    d.conf = fabs(p);

    return d;
}

}
