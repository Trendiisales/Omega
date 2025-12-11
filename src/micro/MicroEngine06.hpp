#pragma once
#include "../market/Tick.hpp"
#include "../market/OrderBook.hpp"
#include "../engine/data/MLLogger.hpp"

namespace Omega {

class MicroEngine06 {
public:
    MicroEngine06();
    void update(const Tick& t, const OrderBook& ob);
    void compute(MicroState& ms);

private:
    double deltaAccel;
    double lastDelta;
    double lastAccel;
};

}
