#pragma once
#include "../market/Tick.hpp"
#include "../market/OrderBook.hpp"
#include "../engine/data/MLLogger.hpp"
#include "../micro/MicroState.hpp"

namespace Omega {

class StrategyQ2_01 {
public:
    StrategyQ2_01();
    double compute(const Tick& t,
                   const OrderBook& ob,
                   const MicroState& ms,
                   const double* base);

private:
    double ema;
};

}
