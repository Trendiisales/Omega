#include "StrategyVQ2_08.hpp"
#include <cmath>
namespace Omega {

Decision StrategyVQ2_08::compute(const Tick& t, const OrderBook& ob, const MicroState& ms)
{
    Decision d;

    double H1 = (ms.gradient * 0.4)
              + (ms.accel * 0.3)
              + (ms.imbalance * 0.2)
              + (ms.ofi * 0.1);

    double H2 = (ms.wave * 0.3)
              + (ms.pressure * 0.4)
              + (ms.volatility * -0.2)
              + ((ob.bidPrice[0] - ob.askPrice[0]) * 0.1);

    double H3 = 0;
    for (int i=0;i<10;i++)
        H3 += (ob.bidSize[i] - ob.askSize[i]) * 0.000001;

    double p = (H1 * 0.45) + (H2 * 0.35) + (H3 * 0.20);

    d.score = p;
    d.side  = (p >= 0 ? Side::Buy : Side::Sell);
    d.conf  = fabs(p);

    return d;
}

}
