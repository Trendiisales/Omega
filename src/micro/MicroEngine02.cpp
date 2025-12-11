#include "MicroEngine02.hpp"
#include <cmath>

namespace Omega {

MicroEngine02::MicroEngine02()
    : imbalance(0)
{}

void MicroEngine02::update(const Tick& t, const OrderBook& ob)
{
    double B = ob.bidSize[0] + ob.bidSize[1];
    double A = ob.askSize[0] + ob.askSize[1];

    if(B + A > 0)
        imbalance = (B - A) / (B + A);
    else
        imbalance = 0;
}

void MicroEngine02::compute(MicroState& ms)
{
    ms.v[1] = imbalance;
}

}
