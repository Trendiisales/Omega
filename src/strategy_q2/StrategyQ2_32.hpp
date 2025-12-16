#pragma once
#include "../market/Tick.hpp"
#include "../market/OrderBook.hpp"
#include "../micro/MicroState.hpp"
namespace Omega {
class StrategyQ2_32 {
public:
    StrategyQ2_32();
    double compute(const Tick&, const OrderBook&, const MicroState&, const double*);
private:
    double smooth;
    double lastMid;
};
}
