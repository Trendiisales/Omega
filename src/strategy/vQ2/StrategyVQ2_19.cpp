#include "StrategyVQ2_19.hpp"
#include <cmath>
namespace Omega {

Decision StrategyVQ2_19::compute(const Tick& t, const OrderBook& ob, const MicroState& ms)
{
    Decision d;

    double G1 = (ms.gradient  * 0.33)
              + (ms.ofi       * 0.22)
              + (ms.wave      * 0.15)
              + (ms.volatility * -0.15)
              + (t.delta      * 0.15);

    double G2 = (ms.accel     * 0.40)
              + (ms.pressure  * 0.40)
              + (ms.imbalance * 0.20);

    double G3 = 0;
    for(int i=0;i<10;i++)
        G3 += (ob.bidSize[i] - ob.askSize[i]) * (i+1) * 0.000001;

    double p = (G1*0.40)+(G2*0.40)+(G3*0.20);

    d.score = p;
    d.side  = (p>=0?Side::Buy:Side::Sell);
    d.conf  = fabs(p);

    return d;
}

}
