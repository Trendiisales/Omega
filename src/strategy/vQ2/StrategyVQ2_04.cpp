#include "StrategyVQ2_04.hpp"
#include <cmath>
namespace Omega {

Decision StrategyVQ2_04::compute(const Tick& t, const OrderBook& ob, const MicroState& ms)
{
    Decision d;

    double L = 0;
    for (int i=0;i<10;i++)
        L += (ob.bidSize[i] - ob.askSize[i]) * (i+1) * 0.000002;
    L += ms.liquidity * 0.5;

    double M = (ms.accel * 0.35)
             + (ms.gradient * 0.35)
             + (t.delta * 0.15)
             + (ms.ofi * 0.15);

    double N = (ms.pressure * 0.5)
             + (ms.imbalance * 0.3)
             + (t.spread * -0.05);

    double p = (L * 0.3) + (M * 0.4) + (N * 0.3);

    d.score = p;
    d.side  = (p >= 0 ? Side::Buy : Side::Sell);
    d.conf  = fabs(p);

    return d;
}

}
