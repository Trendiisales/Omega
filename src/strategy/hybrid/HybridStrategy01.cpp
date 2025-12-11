#include "../../micro/MicroState.hpp"
#include "HybridStrategy01.hpp"
#include <cmath>

namespace Omega {

Decision HybridStrategy01::compute(const Tick& t, const OrderBook& ob, const MicroState& ms)
{
    Decision d;

    double M1 =
        (ms.gradient   * 0.28) +
        (ms.accel      * 0.26) +
        (ms.pressure   * 0.22) +
        (ms.wave       * 0.14) +
        (ms.ofi        * 0.10);

    double OB1 = 0;
    for(int i=0;i<10;i++)
        OB1 += (ob.bidSize[i] - ob.askSize[i]) * (i+1) * 0.0000017;

    double T1 =
        (t.delta  * 0.18) +
        (t.spread * -0.12) +
        (t.buyVol - t.sellVol) * 0.00011;

    double p = (M1 * 0.45) + (OB1 * 0.30) + (T1 * 0.25);

    d.score = p;
    d.side  = (p >= 0 ? Side::Buy : Side::Sell);
    d.score = fabs(p);

    return d;
}

}
