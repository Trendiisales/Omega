#include "StrategyVQ2_27.hpp"
#include <cmath>
namespace Omega {

Decision StrategyVQ2_27::compute(const Tick& t, const OrderBook& ob, const MicroState& ms)
{
    Decision d;

    double X1 = (ms.impact    * 0.30)
              + (ms.gradient  * 0.30)
              + (ms.wave      * 0.20)
              + (ms.ofi       * 0.10)
              + (ms.volatility * -0.10);

    double X2 = (ms.accel     * 0.45)
              + (ms.pressure  * 0.35)
              + (ms.imbalance * 0.20);

    double X3 = 0;
    for(int i=0;i<10;i++)
        X3 += (ob.bidSize[i] - ob.askSize[i]) * (i+1) * 0.0000014;

    double p = (X1*0.4)+(X2*0.4)+(X3*0.2);

    d.score = p;
    d.side  = (p>=0?Side::Buy:Side::Sell);
    d.conf  = fabs(p);
    return d;
}

}
