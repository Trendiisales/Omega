#pragma once
#include "../market/Tick.hpp"
#include "../market/OrderBook.hpp"
#include "../engine/data/MLLogger.hpp"

namespace Omega {

class Strategy18 {
public:
    Strategy18();
    double compute(const Tick& t,
                   const OrderBook& ob,
                   const MicroState& ms);
private:
    double lastBid;
    double lastAsk;
};

}
