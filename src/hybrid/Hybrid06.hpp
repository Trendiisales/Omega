#pragma once
#include "../market/Tick.hpp"
#include "../market/OrderBook.hpp"
#include "../micro/MicroState.hpp"
namespace Omega {
class Hybrid06 {
public:
    Hybrid06();
    double compute(const Tick&, const OrderBook&, const MicroState&,
                   const double* base, const double* q2);
private:
    double acc;
    double var;
    double lastMid;
};
}
