#pragma once
#include "../market/Tick.hpp"
#include "../market/OrderBook.hpp"
#include "../engine/data/MLLogger.hpp"

namespace Omega {

class MicroEngine07 {
public:
    MicroEngine07();
    void update(const Tick& t, const OrderBook& ob);
    void compute(MicroState& ms);

private:
    double imbalance2;
};

}
