#pragma once
#include "../market/Tick.hpp"
#include "../market/OrderBook.hpp"
#include "../micro/MicroState.hpp"
namespace Omega {
class StrategyQ2_16 {
public:
    StrategyQ2_16();
    double compute(const Tick&, const OrderBook&, const MicroState&, const double*);
private:
    double accel;
    double last;
};
}
