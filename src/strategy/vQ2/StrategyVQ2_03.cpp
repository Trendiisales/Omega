#include "StrategyVQ2_03.hpp"
#include <cmath>
namespace Omega {

Decision StrategyVQ2_03::compute(const Tick& t, const OrderBook& ob, const MicroState& ms)
{
    Decision d;

    double A = (ms.gradient * 0.3)
             + (ms.wave * 0.2)
             + (ms.volatility * -0.15)
             + (ms.ofi * 0.25)
             + ((ob.bidSize[0] - ob.askSize[0]) * 0.00003);

    double B = 0;
    for (int i=1;i<10;i++)
        B += ((ob.bidPrice[i] - ob.askPrice[i]) -
              (ob.bidPrice[i-1] - ob.askPrice[i-1])) * 0.03;

    double C = (ms.pressure * 0.4)
             + (t.delta * 0.1)
             + (ms.imbalance * 0.3)
             + (ms.liquidity * 0.1);

    double p = (A * 0.4) + (B * 0.3) + (C * 0.3);

    d.score = p;
    d.side  = (p >= 0 ? Side::Buy : Side::Sell);
    d.conf  = fabs(p);
    return d;
}

}
