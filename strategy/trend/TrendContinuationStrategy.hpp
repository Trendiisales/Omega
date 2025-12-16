// =============================================================================
// TrendContinuationStrategy.hpp - Trend Continuation CRTP Strategy
// =============================================================================
#pragma once

#include "strategy/StrategyBaseCRTP.hpp"
#include "strategy/trend/TrendContinuationConfig.hpp"

namespace chimera {
namespace strategy {

class TrendContinuationStrategy final
    : public StrategyBaseCRTP<TrendContinuationStrategy> {
public:
    explicit TrendContinuationStrategy(const TrendContinuationConfig& cfg) noexcept
        : cfg_(cfg), last_(0.0), init_(false) {}

    inline bool on_tick_impl(const Tick& t, Intent& out) noexcept {
        if (!(t.flags & chimera::market::TICK_HAS_PRICE)) return false;

        if (!init_) {
            last_ = t.price;
            init_ = true;
            return false;
        }

        const double move = t.price - last_;

        // Trend continuation - follow the move
        if (move >= cfg_.min_move) {
            fill_intent(out, t, 0); // buy - continue uptrend
            last_ = t.price;
            return true;
        }

        if (move <= -cfg_.min_move) {
            fill_intent(out, t, 1); // sell - continue downtrend
            last_ = t.price;
            return true;
        }

        last_ = t.price;
        return false;
    }

private:
    TrendContinuationConfig cfg_;
    double last_;
    bool init_;
};

} // namespace strategy
} // namespace chimera
