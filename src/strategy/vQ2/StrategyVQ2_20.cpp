#include "StrategyVQ2_20.hpp"
#include <cmath>
namespace Omega {

Decision StrategyVQ2_20::compute(const Tick& t, const OrderBook& ob, const MicroState& ms)
{
    Decision d;

    double A1 = (ms.pressure  * 0.45)
              + (ms.gradient  * 0.35)
              + (ms.imbalance * 0.20);

    double A2 = (ms.wave       * 0.25)
              + (ms.accel      * 0.25)
              + (ms.volatility * -0.20)
              + (t.spread      * -0.10);

    double A3 = 0;
    for(int i=0;i<10;i++)
        A3 += (ob.bidSize[i] + ob.askSize[i]) * (i+1) * 0.0000012;

    double p = (A1*0.45)+(A2*0.35)+(A3*0.20);

    d.score = p;
    d.side  = (p>=0?Side::Buy:Side::Sell);
    d.conf  = fabs(p);

    return d;
}

}
