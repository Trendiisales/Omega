#include "StrategyVQ2_09.hpp"
#include <cmath>
namespace Omega {

Decision StrategyVQ2_09::compute(const Tick& t, const OrderBook& ob, const MicroState& ms)
{
    Decision d;

    double L = (ms.liquidity * 0.35)
             + (ms.gradient  * 0.25)
             + (ms.ofi       * 0.15)
             + (ms.imbalance * 0.15)
             + (t.delta      * 0.10);

    double M = 0;
    for (int i=1;i<10;i++)
        M += ((ob.bidPrice[i] - ob.askPrice[i]) -
              (ob.bidPrice[i-1] - ob.askPrice[i-1])) * 0.025;

    double H = (ms.wave      * 0.3)
             + (ms.pressure  * 0.4)
             + (ms.accel     * 0.2)
             + (t.spread     * -0.1);

    double p = (L * 0.4) + (M * 0.3) + (H * 0.3);

    d.score = p;
    d.side  = (p >= 0 ? Side::Buy : Side::Sell);
    d.conf  = fabs(p);
    return d;
}

}
