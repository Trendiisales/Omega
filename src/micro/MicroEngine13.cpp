#include "MicroEngine13.hpp"
#include <cmath>

namespace Omega {

MicroEngine13::MicroEngine13()
    : midAccel(0), lastMid(0), lastVel(0)
{}

void MicroEngine13::update(const Tick& t, const OrderBook& ob)
{
    double mid = 0.5*(t.bid + t.ask);
    double vel = mid - lastMid;
    lastMid = mid;

    double acc = vel - lastVel;
    lastVel = vel;

    midAccel = 0.9*midAccel + 0.1*acc;
}

void MicroEngine13::compute(MicroState& ms)
{
    ms.v[12] = midAccel;
}

}
