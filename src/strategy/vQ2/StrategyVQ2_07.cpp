#include "StrategyVQ2_07.hpp"
#include <cmath>
namespace Omega {

Decision StrategyVQ2_07::compute(const Tick& t, const OrderBook& ob, const MicroState& ms)
{
    Decision d;

    double P = (ms.pressure * 0.5)
             + (ms.gradient * 0.3)
             + (ms.imbalance * 0.2);

    double V = (ms.volatility * -0.25)
             + (ms.wave * 0.20)
             + (ms.accel * 0.15);

    double L = 0;
    for (int i=0;i<10;i++)
        L += (ob.bidSize[i] + ob.askSize[i]) * 0.0000012;

    double p = (P * 0.5) + (V * 0.3) + (L * 0.2);

    d.score = p;
    d.side  = (p >= 0 ? Side::Buy : Side::Sell);
    d.conf  = fabs(p);
    return d;
}

}
