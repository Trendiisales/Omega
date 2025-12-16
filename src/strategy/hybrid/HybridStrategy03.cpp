#include "../../micro/MicroState.hpp"
#include "HybridStrategy03.hpp"
#include <cmath>

namespace Omega {

Decision HybridStrategy03::compute(const Tick& t, const OrderBook& ob, const MicroState& ms)
{
    Decision d;

    double m =
        (ms.gradient   * 0.30) +
        (ms.ofi        * 0.20) +
        (ms.imbalance  * 0.18) +
        (ms.wave       * 0.17) +
        (ms.volatility * -0.15);

    double o = 0;
    for(int i=0;i<10;i++)
        o += (ob.bidSize[i] - ob.askSize[i]) * (i+1) * 0.00000155;

    double tblock =
        (t.delta  * 0.20) +
        (t.spread * -0.12) +
        ((t.buyVol - t.sellVol) * 0.00012);

    double p = (m*0.45) + (o*0.30) + (tblock*0.25);

    d.score = p;
    d.side  = (p>=0 ? Side::Buy : Side::Sell);
    d.score = fabs(p);

    return d;
}

}
