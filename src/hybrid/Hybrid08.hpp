#pragma once
#include "../market/Tick.hpp"
#include "../market/OrderBook.hpp"
#include "../micro/MicroState.hpp"
namespace Omega {
class Hybrid08 {
public:
    Hybrid08();
    double compute(const Tick&, const OrderBook&, const MicroState&,
                   const double* base, const double* q2);
private:
    double last;
    double mom;
    double shock;
};
}
