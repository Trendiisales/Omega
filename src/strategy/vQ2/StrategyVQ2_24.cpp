#include "StrategyVQ2_24.hpp"
#include <cmath>
namespace Omega {

Decision StrategyVQ2_24::compute(const Tick& t, const OrderBook& ob, const MicroState& ms)
{
    Decision d;

    double H1 = (ms.gradient  * 0.30)
              + (ms.wave     * 0.20)
              + (ms.volatility * -0.20)
              + (ms.ofi      * 0.20)
              + (ms.imbalance * 0.10);

    double H2 = (ms.accel    * 0.35)
              + (ms.pressure * 0.35)
              + (t.delta     * 0.10)
              + (t.spread    * -0.10);

    double H3 = 0;
    for(int i=0;i<10;i++)
        H3 += (ob.bidSize[i] + ob.askSize[i]) * (i+1) * 0.0000011;

    double p = (H1*0.4)+(H2*0.4)+(H3*0.2);

    d.score = p;
    d.side = (p>=0?Side::Buy:Side::Sell);
    d.conf = fabs(p);
    return d;
}

}
