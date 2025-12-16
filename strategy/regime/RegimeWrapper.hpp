// =============================================================================
// RegimeWrapper.hpp - Compile-Time Strategy Composition
// =============================================================================
// Selects between two strategies based on regime detection.
// No virtuals, no runtime overhead beyond the predicate.
// =============================================================================
#pragma once

#include "strategy/StrategyBaseCRTP.hpp"

namespace chimera {
namespace strategy {

// =============================================================================
// RegimeWrapper
// Compile-time wrapper selecting one of two strategies based on a predicate.
// Derived must implement: bool use_a(const Tick&) const noexcept
// =============================================================================
template <typename Derived, typename StratA, typename StratB>
class RegimeWrapper
    : public StrategyBaseCRTP<RegimeWrapper<Derived, StratA, StratB>> {
public:
    using Tick   = chimera::market::Tick;
    using Intent = chimera::engine::Intent;

    RegimeWrapper(StratA& a, StratB& b) noexcept
        : a_(a), b_(b) {}

    inline bool on_tick_impl(const Tick& t, Intent& out) noexcept {
        if (static_cast<const Derived*>(this)->use_a(t)) {
            return a_.on_tick(t, out);
        }
        return b_.on_tick(t, out);
    }

private:
    StratA& a_;
    StratB& b_;
};

// =============================================================================
// Example: VolatilityRegimeWrapper
// Uses strategy A in high volatility, B in low volatility
// =============================================================================
template <typename StratA, typename StratB>
class VolatilityRegimeWrapper final
    : public RegimeWrapper<VolatilityRegimeWrapper<StratA, StratB>, StratA, StratB> {
public:
    using Base = RegimeWrapper<VolatilityRegimeWrapper<StratA, StratB>, StratA, StratB>;
    using Tick = chimera::market::Tick;

    VolatilityRegimeWrapper(StratA& a, StratB& b, double vol_threshold) noexcept
        : Base(a, b)
        , vol_threshold_(vol_threshold)
        , last_price_(0.0)
        , vol_estimate_(0.0)
        , init_(false)
    {}

    inline bool use_a(const Tick& t) const noexcept {
        return vol_estimate_ >= vol_threshold_;
    }

    // Call this to update volatility estimate
    inline void update_vol(const Tick& t) noexcept {
        if (!(t.flags & chimera::market::TICK_HAS_PRICE)) return;

        if (!init_) {
            last_price_ = t.price;
            init_ = true;
            return;
        }

        const double ret = (t.price - last_price_) / last_price_;
        const double ret_sq = ret * ret;

        // Exponential moving average of squared returns
        constexpr double alpha = 0.1;
        vol_estimate_ = alpha * ret_sq + (1.0 - alpha) * vol_estimate_;

        last_price_ = t.price;
    }

private:
    double vol_threshold_;
    double last_price_;
    double vol_estimate_;
    bool init_;
};

} // namespace strategy
} // namespace chimera
