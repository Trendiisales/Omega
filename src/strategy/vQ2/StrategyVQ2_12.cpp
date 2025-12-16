#include "StrategyVQ2_12.hpp"
#include <cmath>
namespace Omega {

Decision StrategyVQ2_12::compute(const Tick& t, const OrderBook& ob, const MicroState& ms)
{
    Decision d;

    double S1 = (ms.pressure  * 0.5)
              + (ms.gradient  * 0.3)
              + (ms.imbalance * 0.2);

    double S2 = (ms.wave      * 0.25)
              + (ms.accel     * 0.25)
              + (ms.volatility * -0.2)
              + (t.spread      * -0.05);

    double S3 = 0;
    for (int i=0;i<10;i++)
        S3 += (ob.bidSize[i] + ob.askSize[i]) * 0.000001;

    double p = (S1*0.45)+(S2*0.35)+(S3*0.20);

    d.score = p;
    d.side = (p>=0?Side::Buy:Side::Sell);
    d.conf = fabs(p);
    return d;
}

}
