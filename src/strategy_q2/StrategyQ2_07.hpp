#pragma once
#include "../market/Tick.hpp"
#include "../market/OrderBook.hpp"
#include "../micro/MicroState.hpp"
namespace Omega {
class StrategyQ2_07 {
public:
    StrategyQ2_07();
    double compute(const Tick&, const OrderBook&, const MicroState&, const double*);
private:
    double ema;
};
}
