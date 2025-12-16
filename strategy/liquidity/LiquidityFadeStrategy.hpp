// =============================================================================
// LiquidityFadeStrategy.hpp - Liquidity Fade CRTP Strategy
// =============================================================================
// Fades large resting orders - sells into large bids, buys into large asks
// =============================================================================
#pragma once

#include "strategy/StrategyBaseCRTP.hpp"
#include "strategy/liquidity/LiquidityFadeConfig.hpp"

namespace chimera {
namespace strategy {

class LiquidityFadeStrategy final
    : public StrategyBaseCRTP<LiquidityFadeStrategy> {
public:
    explicit LiquidityFadeStrategy(const LiquidityFadeConfig& cfg) noexcept
        : cfg_(cfg) {}

    inline bool on_tick_impl(const Tick& t, Intent& out) noexcept {
        // Only process book updates
        if (!(t.flags & chimera::market::TICK_IS_BOOK)) return false;

        // Check for minimum size
        if (t.size < cfg_.min_size) return false;

        // Fade the liquidity - sell into large bids, buy into large asks
        if (t.side == chimera::market::SIDE_BID) {
            fill_intent(out, t, 1); // sell into large bid
        } else if (t.side == chimera::market::SIDE_ASK) {
            fill_intent(out, t, 0); // buy into large ask
        } else {
            return false;
        }

        return true;
    }

private:
    LiquidityFadeConfig cfg_;
};

} // namespace strategy
} // namespace chimera
