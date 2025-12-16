#include "MicroEngine17.hpp"
#include <cmath>

namespace Omega {

MicroEngine17::MicroEngine17()
    : volatility(0), var(0)
{}

void MicroEngine17::update(const Tick& t, const OrderBook& ob)
{
    double mid = 0.5*(t.bid + t.ask);

    var = 0.95*var + 0.05*(mid*mid);
    volatility = std::sqrt(std::max(0.0, var - mid*mid));
}

void MicroEngine17::compute(MicroState& ms)
{
    ms.v[16] = volatility;
}

}
