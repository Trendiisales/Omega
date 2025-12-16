#include "StrategyVQ2_01.hpp"
#include <cmath>
namespace Omega {

Decision StrategyVQ2_01::compute(const Tick& t, const OrderBook& ob, const MicroState& ms)
{
    Decision d;

    double s1 = (ms.gradient * 0.35)
              + (ms.imbalance * 0.25)
              + (ms.ofi * 0.15)
              + (ms.volatility * -0.1)
              + (ms.liquidity * 0.05)
              + (t.delta * 0.10);

    double s2 = (ms.wave * 0.30)
              + (ms.pressure * 0.40)
              + (ms.accel * 0.20)
              + ((ob.bidPrice[0] - ob.askPrice[0]) * 0.05);

    double s3 = 0;
    for (int i=0;i<10;i++)
        s3 += (ob.bidSize[i] - ob.askSize[i]) * (i+1) * 0.000002;

    double p = (s1 * 0.4) + (s2 * 0.4) + (s3 * 0.2);

    d.score = p;
    d.side  = (p >= 0 ? Side::Buy : Side::Sell);
    d.conf  = fabs(p);
    return d;
}

}
