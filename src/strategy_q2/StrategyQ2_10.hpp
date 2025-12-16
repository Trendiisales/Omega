#pragma once
#include "../market/Tick.hpp"
#include "../market/OrderBook.hpp"
#include "../micro/MicroState.hpp"
namespace Omega {
class StrategyQ2_10 {
public:
    StrategyQ2_10();
    double compute(const Tick&, const OrderBook&, const MicroState&, const double*);
private:
    double shock;
};
}
