#include "../../micro/MicroState.hpp"
#include "HybridStrategy02.hpp"
#include <cmath>

namespace Omega {

Decision HybridStrategy02::compute(const Tick& t, const OrderBook& ob, const MicroState& ms)
{
    Decision d;

    double core =
        (ms.gradient    * 0.32) +
        (ms.accel       * 0.28) +
        (ms.pressure    * 0.22) +
        (ms.imbalance   * 0.18);

    double depth = 0;
    for(int i=0;i<10;i++)
        depth += (ob.bidSize[i] + ob.askSize[i]) * (i+1) * 0.00000145;

    double trend =
        (ms.wave       * 0.22) +
        (ms.volatility * -0.18) +
        (t.delta       * 0.14) +
        (t.spread      * -0.10);

    double p = (core*0.40) + (depth*0.35) + (trend*0.25);

    d.score = p;
    d.side = (p>=0 ? Side::Buy : Side::Sell);
    d.score = fabs(p);

    return d;
}

}
