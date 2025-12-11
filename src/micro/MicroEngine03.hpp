#pragma once
#include "../market/Tick.hpp"
#include "../market/OrderBook.hpp"
#include "../engine/data/MLLogger.hpp"

namespace Omega {

class MicroEngine03 {
public:
    MicroEngine03();
    void update(const Tick& t, const OrderBook& ob);
    void compute(MicroState& ms);

private:
    double volEMA;
    double lastVol;
};

}
