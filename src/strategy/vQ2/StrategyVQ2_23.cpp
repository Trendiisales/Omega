#include "StrategyVQ2_23.hpp"
#include <cmath>
namespace Omega {

Decision StrategyVQ2_23::compute(const Tick& t, const OrderBook& ob, const MicroState& ms)
{
    Decision d;

    double M1 = (ms.liquidity * 0.28)
              + (ms.gradient  * 0.32)
              + (ms.imbalance * 0.20)
              + (ms.ofi       * 0.20);

    double M2 = (ms.accel     * 0.40)
              + (ms.pressure  * 0.40)
              + (ms.wave      * 0.20);

    double M3 = 0;
    for(int i=0;i<10;i++)
        M3 += (ob.bidSize[i] - ob.askSize[i]) * (i+1) * 0.0000014;

    double p = (M1*0.40)+(M2*0.40)+(M3*0.20);

    d.score = p;
    d.side  = (p>=0?Side::Buy:Side::Sell);
    d.conf  = fabs(p);
    return d;
}

}
