#include "MicroEngine10.hpp"
#include <cmath>

namespace Omega {

MicroEngine10::MicroEngine10()
    : depthGradient(0)
{}

void MicroEngine10::update(const Tick& t, const OrderBook& ob)
{
    double b0 = ob.bidSize[0];
    double b1 = ob.bidSize[1];
    double b2 = ob.bidSize[2];
    double a0 = ob.askSize[0];
    double a1 = ob.askSize[1];
    double a2 = ob.askSize[2];

    double bSlope = (b0 + b1 + b2);
    double aSlope = (a0 + a1 + a2);

    if(bSlope + aSlope > 0)
        depthGradient = (bSlope - aSlope) / (bSlope + aSlope);
    else
        depthGradient = 0;
}

void MicroEngine10::compute(MicroState& ms)
{
    ms.v[9] = depthGradient;
}

}
