#include "MicroEngine07.hpp"
#include <cmath>

namespace Omega {

MicroEngine07::MicroEngine07()
    : imbalance2(0)
{}

void MicroEngine07::update(const Tick& t, const OrderBook& ob)
{
    double B = ob.bidSize[0] + ob.bidSize[2] + ob.bidSize[4];
    double A = ob.askSize[0] + ob.askSize[2] + ob.askSize[4];

    if(B + A > 0)
        imbalance2 = (B - A) / (B + A);
    else
        imbalance2 = 0;
}

void MicroEngine07::compute(MicroState& ms)
{
    ms.v[6] = imbalance2;
}

}
