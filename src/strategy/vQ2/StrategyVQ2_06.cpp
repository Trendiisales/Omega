#include "StrategyVQ2_06.hpp"
#include <cmath>
namespace Omega {

Decision StrategyVQ2_06::compute(const Tick& t, const OrderBook& ob, const MicroState& ms)
{
    Decision d;

    double X1 = (ms.impact * 0.3)
              + (ms.gradient * 0.25)
              + (ms.wave * 0.20)
              + (ms.ofi * 0.15)
              + (ms.imbalance * 0.10);

    double X2 = (ms.accel * 0.4)
              + (ms.volatility * -0.2)
              + (ms.liquidity * 0.15)
              + (t.delta * 0.05);

    double X3 = 0;
    for (int i=0;i<10;i++)
        X3 += (ob.bidSize[i] - ob.askSize[i]) * (i+1) * 0.0000015;

    double p = (X1 * 0.45) + (X2 * 0.35) + (X3 * 0.20);

    d.score = p;
    d.side  = (p >= 0 ? Side::Buy : Side::Sell);
    d.conf  = fabs(p);

    return d;
}

}
