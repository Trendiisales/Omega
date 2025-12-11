#include "../../micro/MicroState.hpp"
#include "HybridStrategy05.hpp"
#include <cmath>
namespace Omega {

Decision HybridStrategy05::compute(const Tick& t, const OrderBook& ob, const MicroState& ms)
{
    Decision d;

    double macro =
        (ms.gradient   * 0.34) +
        (ms.accel      * 0.26) +
        (ms.pressure   * 0.24) +
        (ms.ofi        * 0.16);

    double depth = 0;
    for(int i=0;i<10;i++)
        depth += (ob.bidSize[i] - ob.askSize[i]) * (i+1) * 0.00000155;

    double tempo =
        (t.delta      * 0.18) +
        (t.spread     * -0.15) +
        ((t.buyVol - t.sellVol) * 0.00010);

    double p = (macro*0.43)+(depth*0.32)+(tempo*0.25);

    d.score = p;
    d.side = (p>=0 ? Side::Buy : Side::Sell);
    d.score = fabs(p);
    return d;
}

}
