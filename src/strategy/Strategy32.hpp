#pragma once
#include "../market/Tick.hpp"
#include "../market/OrderBook.hpp"
#include "../engine/data/MLLogger.hpp"

namespace Omega {

class Strategy32 {
public:
    Strategy32();
    double compute(const Tick& t,
                   const OrderBook& ob,
                   const MicroState& ms);

private:
    double smooth;
    double lastTradePrice;
};

}
