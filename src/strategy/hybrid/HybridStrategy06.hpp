#pragma once
#include "../../market/Tick.hpp"
#include "../../market/OrderBook.hpp"
#include "../../micro/MicroState.hpp"
#include "../../strategy/Decision.hpp"

namespace Omega {
class HybridStrategy06 {
public:
    Decision compute(const Tick&, const OrderBook&, const MicroState&);
};
}
