#include "StrategyVQ2_25.hpp"
#include <cmath>
namespace Omega {

Decision StrategyVQ2_25::compute(const Tick& t, const OrderBook& ob, const MicroState& ms)
{
    Decision d;

    double A = (ms.gradient   * 0.30)
             + (ms.imbalance  * 0.25)
             + (ms.ofi        * 0.20)
             + (ms.wave       * 0.15)
             + (ms.volatility * -0.10);

    double B = (ms.accel      * 0.40)
             + (ms.pressure   * 0.40)
             + (ms.liquidity  * 0.20);

    double C = 0;
    for(int i=0;i<10;i++)
        C += (ob.bidSize[i] - ob.askSize[i]) * (i+1) * 0.00000125;

    double p = (A*0.4)+(B*0.4)+(C*0.2);

    d.score = p;
    d.side  = (p>=0? Side::Buy : Side::Sell);
    d.conf  = fabs(p);
    return d;
}

}
