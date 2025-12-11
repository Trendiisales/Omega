#include "StrategyVQ2_02.hpp"
#include <cmath>
namespace Omega {

Decision StrategyVQ2_02::compute(const Tick& t, const OrderBook& ob, const MicroState& ms)
{
    Decision d;

    double L1 = (ms.impact * 0.5)
              + (ms.liquidity * 0.2)
              + (ms.gradient * 0.2)
              + (t.spread * -0.1);

    double L2 = (ms.accel * 0.4)
              + (ms.ofi * 0.3)
              + (ms.imbalance * 0.2)
              + ((t.buyVol - t.sellVol) * 0.1);

    double L3 = 0;
    for (int i=0;i<10;i++)
        L3 += ((ob.bidSize[i] + ob.askSize[i]) * (i+1) * 0.000001);

    double p = (L1 * 0.5) + (L2 * 0.3) + (L3 * 0.2);

    d.score = p;
    d.side  = (p > 0 ? Side::Buy : Side::Sell);
    d.conf  = fabs(p);
    return d;
}

}
