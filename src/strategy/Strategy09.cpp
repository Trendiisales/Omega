#include "Strategy09.hpp"
#include <cmath>

namespace Omega {

Strategy09::Strategy09()
    : lastSpread(0)
{}

double Strategy09::compute(const Tick& t,
                           const OrderBook& ob,
                           const MicroState& ms)
{
    double ds = t.spread - lastSpread;
    lastSpread = t.spread;

    double obPressure = 0;
    double B = ob.bidSize[0] + ob.bidSize[1];
    double A = ob.askSize[0] + ob.askSize[1];
    if(B + A > 0)
        obPressure = (B - A) / (B + A);

    double lv = ms.v[20];

    return ds*0.3 + obPressure*0.4 + lv*0.3;
}

}
