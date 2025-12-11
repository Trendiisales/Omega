#pragma once
#include "../market/Tick.hpp"
#include "../market/OrderBook.hpp"
#include "../micro/MicroState.hpp"
namespace Omega {
class Hybrid01 {
public:
    Hybrid01();
    double compute(const Tick&, const OrderBook&, const MicroState&,
                   const double* base, const double* q2);
private:
    double emaFast;
    double emaSlow;
    double var;
};
}
