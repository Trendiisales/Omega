#pragma once
#include "../market/Tick.hpp"
#include "../market/OrderBook.hpp"
#include "../engine/data/MLLogger.hpp"

namespace Omega {

class Strategy24 {
public:
    Strategy24();
    double compute(const Tick& t,
                   const OrderBook& ob,
                   const MicroState& ms);

private:
    double lastBid;
    double lastAsk;
    double spreadEMA;
};

}
