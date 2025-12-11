#include "MicroEngine14.hpp"
#include <cmath>

namespace Omega {

MicroEngine14::MicroEngine14()
    : depthSym(0)
{}

void MicroEngine14::update(const Tick& t, const OrderBook& ob)
{
    double Bl = ob.bidSize[0] + ob.bidSize[1] + ob.bidSize[2] + ob.bidSize[3] + ob.bidSize[4];
    double Al = ob.askSize[0] + ob.askSize[1] + ob.askSize[2] + ob.askSize[3] + ob.askSize[4];

    if(Bl + Al > 0)
        depthSym = std::abs(Bl - Al) / (Bl + Al);
    else
        depthSym = 0;
}

void MicroEngine14::compute(MicroState& ms)
{
    ms.v[13] = depthSym;
}

}
