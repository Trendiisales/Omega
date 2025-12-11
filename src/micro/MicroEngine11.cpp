#include "MicroEngine11.hpp"
#include <cmath>

namespace Omega {

MicroEngine11::MicroEngine11()
    : shortTermMom(0), lastMid(0)
{}

void MicroEngine11::update(const Tick& t, const OrderBook& ob)
{
    double mid = 0.5*(t.bid + t.ask);
    double d = mid - lastMid;
    lastMid = mid;

    shortTermMom = 0.8*shortTermMom + 0.2*d;
}

void MicroEngine11::compute(MicroState& ms)
{
    ms.v[10] = shortTermMom;
}

}
