#include "../../micro/MicroState.hpp"
#include "HybridStrategy07.hpp"
#include <cmath>
namespace Omega {

Decision HybridStrategy07::compute(const Tick& t, const OrderBook& ob, const MicroState& ms)
{
    Decision d;

    double part1 =
        (ms.gradient   * 0.33) +
        (ms.accel      * 0.27) +
        (ms.pressure   * 0.23) +
        (ms.wave       * 0.17);

    double obv = 0;
    for(int i=0;i<10;i++)
        obv += (ob.bidSize[i] - ob.askSize[i]) * (i+1) * 0.00000175;

    double part2 =
        (t.delta  * 0.22) +
        (t.spread * -0.13) +
        ((t.buyVol - t.sellVol) * 0.00011);

    double p = (part1*0.45) + (obv*0.30) + (part2*0.25);

    d.score = p;
    d.side  = (p>=0 ? Side::Buy : Side::Sell);
    d.score = fabs(p);
    return d;
}

}
