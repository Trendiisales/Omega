#include "../../micro/MicroState.hpp"
#include "HybridStrategy08.hpp"
#include <cmath>
namespace Omega {

Decision HybridStrategy08::compute(const Tick& t, const OrderBook& ob, const MicroState& ms)
{
    Decision d;

    double H1 =
        (ms.gradient   * 0.31) +
        (ms.wave       * 0.21) +
        (ms.ofi        * 0.19) +
        (ms.volatility * -0.17) +
        (ms.imbalance  * 0.12);

    double H2 = (ms.accel * 0.40)
              + (ms.pressure * 0.40)
              + (t.delta * 0.10)
              + (t.spread * -0.10);

    double H3 = 0;
    for(int i=0;i<10;i++)
        H3 += (ob.bidSize[i] + ob.askSize[i]) * (i+1) * 0.00000145;

    double p = (H1*0.40) + (H2*0.40) + (H3*0.20);

    d.score = p;
    d.side  = (p>=0 ? Side::Buy : Side::Sell);
    d.score = fabs(p);
    return d;
}

}
