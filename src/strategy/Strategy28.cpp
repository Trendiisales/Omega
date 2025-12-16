#include "Strategy28.hpp"
#include <cmath>

namespace Omega {

Strategy28::Strategy28()
    : ema(0)
{}

double Strategy28::compute(const Tick& t,
                           const OrderBook& ob,
                           const MicroState& ms)
{
    double mid = 0.5*(t.bid + t.ask);
    ema = 0.8*ema + 0.2*mid;

    double dev = mid - ema;

    double skew = 0;
    double B = ob.bidSize[3] + ob.bidSize[4];
    double A = ob.askSize[3] + ob.askSize[4];
    if(B + A > 0)
        skew = (B - A) / (B + A);

    double micro = ms.v[46];

    return dev*0.45 + skew*0.35 + micro*0.2;
}

}
