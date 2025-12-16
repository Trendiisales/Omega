#include "StrategyVQ2_10.hpp"
#include <cmath>
namespace Omega {

Decision StrategyVQ2_10::compute(const Tick& t, const OrderBook& ob, const MicroState& ms)
{
    Decision d;

    double A = (ms.gradient   * 0.30)
             + (ms.liquidity  * 0.20)
             + (ms.volatility * -0.15)
             + (ms.ofi        * 0.25)
             + (ms.imbalance  * 0.10);

    double B = (ms.accel   * 0.40)
             + (ms.wave    * 0.20)
             + (ms.pressure * 0.30)
             + (t.delta     * 0.10);

    double C = 0;
    for (int i=0;i<10;i++)
        C += (ob.bidSize[i] + ob.askSize[i]) * 0.0000015;

    double p = (A * 0.35) + (B * 0.45) + (C * 0.20);

    d.score = p;
    d.side  = (p>=0?Side::Buy:Side::Sell);
    d.conf  = fabs(p);
    return d;
}

}
