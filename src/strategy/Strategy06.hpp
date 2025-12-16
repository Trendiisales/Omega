#pragma once
#include "../market/Tick.hpp"
#include "../market/OrderBook.hpp"
#include "../engine/data/MLLogger.hpp"

namespace Omega {

class Strategy06 {
public:
    Strategy06();
    double compute(const Tick& t,
                   const OrderBook& ob,
                   const MicroState& ms);

private:
    double lastMid;
    double lastVol;
};

}
