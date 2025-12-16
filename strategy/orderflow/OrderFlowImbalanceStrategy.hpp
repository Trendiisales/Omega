// =============================================================================
// OrderFlowImbalanceStrategy.hpp - Order Flow Imbalance CRTP Strategy
// =============================================================================
#pragma once

#include "strategy/StrategyBaseCRTP.hpp"
#include "strategy/orderflow/OrderFlowImbalanceConfig.hpp"

namespace chimera {
namespace strategy {

class OrderFlowImbalanceStrategy final
    : public StrategyBaseCRTP<OrderFlowImbalanceStrategy> {
public:
    explicit OrderFlowImbalanceStrategy(const OrderFlowImbalanceConfig& cfg) noexcept
        : cfg_(cfg), buys_(0.0), sells_(0.0) {}

    inline bool on_tick_impl(const Tick& t, Intent& out) noexcept {
        // Only process trades
        if (!(t.flags & chimera::market::TICK_IS_TRADE)) return false;

        // Accumulate volume by aggressor side
        if (t.flags & chimera::market::TICK_IS_AGGRESSOR) {
            // Aggressor buyer
            buys_ += t.size;
        } else {
            // Aggressor seller
            sells_ += t.size;
        }

        // Check for buy imbalance
        if (sells_ > 0.0 && (buys_ / sells_) >= cfg_.ratio) {
            fill_intent(out, t, 0); // buy
            buys_ = sells_ = 0.0;
            return true;
        }

        // Check for sell imbalance
        if (buys_ > 0.0 && (sells_ / buys_) >= cfg_.ratio) {
            fill_intent(out, t, 1); // sell
            buys_ = sells_ = 0.0;
            return true;
        }

        return false;
    }

private:
    OrderFlowImbalanceConfig cfg_;
    double buys_;
    double sells_;
};

} // namespace strategy
} // namespace chimera
