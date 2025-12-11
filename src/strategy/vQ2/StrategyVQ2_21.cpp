#include "StrategyVQ2_21.hpp"
#include <cmath>
namespace Omega {

Decision StrategyVQ2_21::compute(const Tick& t, const OrderBook& ob, const MicroState& ms)
{
    Decision d;

    double R1 = (ms.liquidity  * 0.30)
              + (ms.gradient   * 0.30)
              + (ms.ofi        * 0.20)
              + (ms.imbalance  * 0.20);

    double R2 = (ms.accel      * 0.40)
              + (ms.pressure   * 0.40)
              + (ms.wave       * 0.20);

    double R3 = 0;
    for(int i=0;i<10;i++)
        R3 += (ob.bidSize[i] - ob.askSize[i]) * (i+1) * 0.0000013;

    double p = (R1*0.40)+(R2*0.40)+(R3*0.20);

    d.score = p;
    d.side  = (p>=0?Side::Buy:Side::Sell);
    d.conf  = fabs(p);

    return d;
}

}
