// =============================================================================
// BreakoutStrategy.hpp - Breakout CRTP Strategy
// =============================================================================
#pragma once

#include "strategy/StrategyBaseCRTP.hpp"
#include "strategy/breakout/BreakoutConfig.hpp"

namespace chimera {
namespace strategy {

class BreakoutStrategy final
    : public StrategyBaseCRTP<BreakoutStrategy> {
public:
    explicit BreakoutStrategy(const BreakoutConfig& cfg) noexcept
        : cfg_(cfg), hi_(0.0), lo_(0.0), init_(false) {}

    inline bool on_tick_impl(const Tick& t, Intent& out) noexcept {
        if (!(t.flags & chimera::market::TICK_HAS_PRICE)) return false;

        if (!init_) {
            hi_ = lo_ = t.price;
            init_ = true;
            return false;
        }

        // Breakout above range
        if (t.price > hi_ + cfg_.range) {
            fill_intent(out, t, 0); // buy
            hi_ = lo_ = t.price;
            return true;
        }

        // Breakout below range
        if (t.price < lo_ - cfg_.range) {
            fill_intent(out, t, 1); // sell
            hi_ = lo_ = t.price;
            return true;
        }

        // Update range
        if (t.price > hi_) hi_ = t.price;
        if (t.price < lo_) lo_ = t.price;

        return false;
    }

private:
    BreakoutConfig cfg_;
    double hi_, lo_;
    bool init_;
};

} // namespace strategy
} // namespace chimera
