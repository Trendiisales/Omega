#include "MicroEngine16.hpp"
#include <cmath>

namespace Omega {

MicroEngine16::MicroEngine16()
    : bookPressure(0)
{}

void MicroEngine16::update(const Tick& t, const OrderBook& ob)
{
    double B = ob.bidSize[0] + ob.bidSize[1] + ob.bidSize[2] +
               ob.bidSize[3] + ob.bidSize[4];
    double A = ob.askSize[0] + ob.askSize[1] + ob.askSize[2] +
               ob.askSize[3] + ob.askSize[4];

    if(B + A > 0)
        bookPressure = (B - A) / (B + A);
    else
        bookPressure = 0;
}

void MicroEngine16::compute(MicroState& ms)
{
    ms.v[15] = bookPressure;
}

}
