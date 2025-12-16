#include "Strategy20.hpp"
#include <cmath>

namespace Omega {

Strategy20::Strategy20()
    : mom(0)
{}

double Strategy20::compute(const Tick& t,
                           const OrderBook& ob,
                           const MicroState& ms)
{
    double mid = 0.5*(t.bid + t.ask);

    mom = 0.9*mom + 0.1*(mid * 0.0001);

    double lvlTilt = 0;
    double B = ob.bidSize[1] + ob.bidSize[3] + ob.bidSize[5];
    double A = ob.askSize[1] + ob.askSize[3] + ob.askSize[5];
    if(B + A > 0)
        lvlTilt = (B - A) / (B + A);

    double micro = (ms.v[36] + ms.v[37]) * 0.5;

    return mom*0.5 + lvlTilt*0.3 + micro*0.2;
}

}
