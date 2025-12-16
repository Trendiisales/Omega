#pragma once
#include "../../market/Tick.hpp"
#include "../../market/OrderBook.hpp"
#include "../../micro/MicroState.hpp"
#include "../../strategy/Decision.hpp"
namespace Omega {
class StrategyVQ2_04 {
public:
    Decision compute(const Tick& t, const OrderBook& ob, const MicroState& ms);
};
}
