#include "../../micro/MicroState.hpp"
#include "HybridStrategy04.hpp"
#include <cmath>
namespace Omega {

Decision HybridStrategy04::compute(const Tick& t, const OrderBook& ob, const MicroState& ms)
{
    Decision d;

    double M =
        (ms.gradient   * 0.31) +
        (ms.accel      * 0.24) +
        (ms.pressure   * 0.23) +
        (ms.wave       * 0.12) +
        (ms.ofi        * 0.10);

    double OB = 0;
    for(int i=0;i<10;i++)
        OB += (ob.bidSize[i] + ob.askSize[i]) * (i+1) * 0.0000016;

    double T =
        (t.delta  * 0.20) +
        (t.spread * -0.15) +
        ((t.buyVol - t.sellVol) * 0.00013);

    double p = (M*0.42) + (OB*0.33) + (T*0.25);

    d.score = p;
    d.side  = (p>=0 ? Side::Buy : Side::Sell);
    d.score = fabs(p);
    return d;
}

}
