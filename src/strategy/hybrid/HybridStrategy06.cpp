#include "../../micro/MicroState.hpp"
#include "HybridStrategy06.hpp"
#include <cmath>
namespace Omega {

Decision HybridStrategy06::compute(const Tick& t, const OrderBook& ob, const MicroState& ms)
{
    Decision d;

    double msig =
        (ms.gradient   * 0.29) +
        (ms.wave       * 0.23) +
        (ms.accel      * 0.22) +
        (ms.imbalance  * 0.16) +
        (ms.volatility * -0.10);

    double obsig = 0;
    for(int i=0;i<10;i++)
        obsig += (ob.bidSize[i] + ob.askSize[i]) * (i+1) * 0.0000017;

    double tsig =
        (t.delta  * 0.20) +
        (t.spread * -0.12) +
        ((t.buyVol - t.sellVol) * 0.00009);

    double p = (msig*0.44) + (obsig*0.33) + (tsig*0.23);

    d.score = p;
    d.side  = (p>=0 ? Side::Buy : Side::Sell);
    d.score = fabs(p);
    return d;
}

}
