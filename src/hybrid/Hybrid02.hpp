#pragma once
#include "../market/Tick.hpp"
#include "../market/OrderBook.hpp"
#include "../micro/MicroState.hpp"
namespace Omega {
class Hybrid02 {
public:
    Hybrid02();
    double compute(const Tick&, const OrderBook&, const MicroState&,
                   const double* base, const double* q2);
private:
    double lastMid;
    double momentum;
    double shock;
};
}
