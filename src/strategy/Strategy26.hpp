#pragma once
#include "../market/Tick.hpp"
#include "../market/OrderBook.hpp"
#include "../engine/data/MLLogger.hpp"

namespace Omega {

class Strategy26 {
public:
    Strategy26();
    double compute(const Tick& t,
                   const OrderBook& ob,
                   const MicroState& ms);
private:
    double trend;
    double lastMid;
};

}
