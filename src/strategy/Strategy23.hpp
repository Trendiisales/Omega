#pragma once
#include "../market/Tick.hpp"
#include "../market/OrderBook.hpp"
#include "../engine/data/MLLogger.hpp"

namespace Omega {

class Strategy23 {
public:
    Strategy23();
    double compute(const Tick& t,
                   const OrderBook& ob,
                   const MicroState& ms);
private:
    double lastVol;
    double volEMA;
};

}
