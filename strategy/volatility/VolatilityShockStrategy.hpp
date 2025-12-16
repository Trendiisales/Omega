// =============================================================================
// VolatilityShockStrategy.hpp - Volatility Shock CRTP Strategy
// =============================================================================
#pragma once

#include "strategy/StrategyBaseCRTP.hpp"
#include "strategy/volatility/VolatilityShockConfig.hpp"

namespace chimera {
namespace strategy {

class VolatilityShockStrategy final
    : public StrategyBaseCRTP<VolatilityShockStrategy> {
public:
    explicit VolatilityShockStrategy(const VolatilityShockConfig& cfg) noexcept
        : cfg_(cfg), last_(0.0), init_(false) {}

    inline bool on_tick_impl(const Tick& t, Intent& out) noexcept {
        if (!(t.flags & chimera::market::TICK_HAS_PRICE)) return false;

        if (!init_) {
            last_ = t.price;
            init_ = true;
            return false;
        }

        const double move = t.price - last_;

        // Upward shock
        if (move >= cfg_.shock) {
            fill_intent(out, t, 0); // buy on upward momentum
            last_ = t.price;
            return true;
        }

        // Downward shock
        if (move <= -cfg_.shock) {
            fill_intent(out, t, 1); // sell on downward momentum
            last_ = t.price;
            return true;
        }

        last_ = t.price;
        return false;
    }

private:
    VolatilityShockConfig cfg_;
    double last_;
    bool init_;
};

} // namespace strategy
} // namespace chimera
