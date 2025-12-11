#include "StrategyVQ2_05.hpp"
#include <cmath>
namespace Omega {

Decision StrategyVQ2_05::compute(const Tick& t, const OrderBook& ob, const MicroState& ms)
{
    Decision d;

    double A = (ms.wave * 0.3)
             + (ms.gradient * 0.3)
             + (ms.volatility * -0.2)
             + (ms.liquidity * 0.1)
             + (t.delta * 0.1);

    double B = (ms.accel * 0.4)
             + (ms.ofi * 0.2)
             + (ms.pressure * 0.3)
             + ((ob.bidPrice[0] - ob.askPrice[0]) * 0.1);

    double C = 0;
    for (int i=0;i<10;i++)
        C += (ob.bidSize[i] + ob.askSize[i]) * 0.000001;

    double p = (A * 0.35) + (B * 0.45) + (C * 0.20);

    d.score = p;
    d.side  = (p >= 0 ? Side::Buy : Side::Sell);
    d.conf  = fabs(p);
    return d;
}

}
