#pragma once
#include "../market/Tick.hpp"
#include "../market/OrderBook.hpp"
#include "../micro/MicroState.hpp"

namespace Omega {

class StrategyQ2_05 {
public:
    StrategyQ2_05();
    double compute(const Tick& t,
                   const OrderBook& ob,
                   const MicroState& ms,
                   const double* base);

private:
    double lastMid;
    double accel;
};

}
