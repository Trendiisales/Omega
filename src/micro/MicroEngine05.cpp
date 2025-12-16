#include "MicroEngine05.hpp"
#include <cmath>

namespace Omega {

MicroEngine05::MicroEngine05()
    : depthTilt(0)
{}

void MicroEngine05::update(const Tick& t, const OrderBook& ob)
{
    double B = ob.bidSize[0] + ob.bidSize[1] + ob.bidSize[2];
    double A = ob.askSize[0] + ob.askSize[1] + ob.askSize[2];

    if(B + A > 0)
        depthTilt = (B - A) / (B + A);
    else
        depthTilt = 0;
}

void MicroEngine05::compute(MicroState& ms)
{
    ms.v[4] = depthTilt;
}

}
