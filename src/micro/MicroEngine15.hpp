#pragma once
#include "../market/Tick.hpp"
#include "../market/OrderBook.hpp"
#include "../engine/data/MLLogger.hpp"

namespace Omega {

class MicroEngine15 {
public:
    MicroEngine15();
    void update(const Tick& t, const OrderBook& ob);
    void compute(MicroState& ms);

private:
    double spreadTrend;
    double lastSpread;
};

}
