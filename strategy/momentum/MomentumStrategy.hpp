// =============================================================================
// MomentumStrategy.hpp - Momentum CRTP Strategy
// =============================================================================
#pragma once

#include "strategy/StrategyBaseCRTP.hpp"
#include "strategy/momentum/MomentumConfig.hpp"

namespace chimera {
namespace strategy {

class MomentumStrategy final
    : public StrategyBaseCRTP<MomentumStrategy> {
public:
    explicit MomentumStrategy(const MomentumConfig& cfg) noexcept
        : cfg_(cfg), last_price_(0.0), ticks_(0) {}

    inline bool on_tick_impl(const Tick& t, Intent& out) noexcept {
        if (!(t.flags & chimera::market::TICK_HAS_PRICE)) return false;

        if (ticks_ == 0) {
            last_price_ = t.price;
            ticks_ = 1;
            return false;
        }

        const double dp = t.price - last_price_;
        ++ticks_;

        if (ticks_ >= cfg_.min_ticks && dp >= cfg_.threshold) {
            fill_intent(out, t, 0); // buy
            reset_(t.price);
            return true;
        }

        if (ticks_ >= cfg_.min_ticks && dp <= -cfg_.threshold) {
            fill_intent(out, t, 1); // sell
            reset_(t.price);
            return true;
        }

        last_price_ = t.price;
        return false;
    }

private:
    inline void reset_(double price) noexcept {
        last_price_ = price;
        ticks_ = 0;
    }

    MomentumConfig cfg_;
    double last_price_;
    uint32_t ticks_;
};

} // namespace strategy
} // namespace chimera
