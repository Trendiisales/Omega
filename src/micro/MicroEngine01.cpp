#include "MicroEngine01.hpp"
#include <cmath>

namespace Omega {

MicroEngine01::MicroEngine01()
    : lastMid(0), momentum(0)
{}

void MicroEngine01::update(const Tick& t, const OrderBook& ob)
{
    double mid = 0.5*(t.bid + t.ask);
    double d = mid - lastMid;
    lastMid = mid;

    momentum = 0.9*momentum + 0.1*d;
}

void MicroEngine01::compute(MicroState& ms)
{
    ms.v[0] = momentum;
}

}
