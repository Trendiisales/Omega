#pragma once
#include "../market/Tick.hpp"
#include "../market/OrderBook.hpp"
#include "../micro/MicroState.hpp"
namespace Omega {
class Hybrid07 {
public:
    Hybrid07();
    double compute(const Tick&, const OrderBook&, const MicroState&,
                   const double* base, const double* q2);
private:
    double drift;
    double mavg;
    double last;
};
}
