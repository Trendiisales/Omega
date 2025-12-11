#pragma once
#include "../market/Tick.hpp"
#include "../market/OrderBook.hpp"
#include "../engine/data/MLLogger.hpp"

namespace Omega {

class MicroEngine16 {
public:
    MicroEngine16();
    void update(const Tick& t, const OrderBook& ob);
    void compute(MicroState& ms);

private:
    double bookPressure;
};

}
