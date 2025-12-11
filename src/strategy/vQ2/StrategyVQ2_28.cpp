#include "StrategyVQ2_28.hpp"
#include <cmath>
namespace Omega {

Decision StrategyVQ2_28::compute(const Tick& t, const OrderBook& ob, const MicroState& ms)
{
    Decision d;

    double F1 = (ms.gradient * 0.33)
              + (ms.wave     * 0.22)
              + (ms.ofi      * 0.20)
              + (ms.volatility * -0.15)
              + (t.delta     * 0.10);

    double F2 = (ms.accel     * 0.40)
              + (ms.pressure  * 0.40)
              + (ms.imbalance * 0.20);

    double F3 = 0;
    for(int i=0;i<10;i++)
        F3 += (ob.bidSize[i] + ob.askSize[i]) * (i+1) * 0.0000013;

    double p = (F1*0.35)+(F2*0.45)+(F3*0.20);

    d.score = p;
    d.side = (p>=0?Side::Buy:Side::Sell);
    d.conf = fabs(p);
    return d;
}

}
