#pragma once
#include "../market/Tick.hpp"
#include "../market/OrderBook.hpp"
#include "../engine/data/MLLogger.hpp"

namespace Omega {

class MicroEngine01 {
public:
    MicroEngine01();
    void update(const Tick& t, const OrderBook& ob);
    void compute(MicroState& ms);

private:
    double lastMid;
    double momentum;
};

}
