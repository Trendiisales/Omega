#pragma once
#include "../market/Tick.hpp"
#include "../market/OrderBook.hpp"
#include "../engine/data/MLLogger.hpp"

namespace Omega {

class Strategy07 {
public:
    Strategy07();
    double compute(const Tick& t,
                   const OrderBook& ob,
                   const MicroState& ms);
private:
    double lastMid;
    double lastDelta;
};

}
