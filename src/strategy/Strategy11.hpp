#pragma once
#include "../market/Tick.hpp"
#include "../market/OrderBook.hpp"
#include "../engine/data/MLLogger.hpp"

namespace Omega {

class Strategy11 {
public:
    Strategy11();
    double compute(const Tick& t,
                   const OrderBook& ob,
                   const MicroState& ms);

private:
    double avgSpread;
};

}
