// =============================================================================
// MeanReversionStrategy.hpp - Mean Reversion CRTP Strategy
// =============================================================================
// FIXED: Uses windowed rolling mean instead of unbounded incremental mean
// =============================================================================
#pragma once

#include "strategy/StrategyBaseCRTP.hpp"
#include "strategy/reversion/MeanReversionConfig.hpp"

namespace chimera {
namespace strategy {

class MeanReversionStrategy final
    : public StrategyBaseCRTP<MeanReversionStrategy> {
public:
    static constexpr uint32_t MAX_WINDOW = 256;

    explicit MeanReversionStrategy(const MeanReversionConfig& cfg) noexcept
        : cfg_(cfg)
        , sum_(0.0)
        , head_(0)
        , count_(0)
    {
        // Zero initialize price buffer
        for (uint32_t i = 0; i < MAX_WINDOW; ++i) {
            prices_[i] = 0.0;
        }
    }

    inline bool on_tick_impl(const Tick& t, Intent& out) noexcept {
        if (!(t.flags & chimera::market::TICK_HAS_PRICE)) return false;

        const uint32_t window = (cfg_.window < MAX_WINDOW) ? cfg_.window : MAX_WINDOW;

        // Add new price to rolling window
        if (count_ >= window) {
            // Remove oldest price from sum
            sum_ -= prices_[head_];
        }

        // Store new price
        prices_[head_] = t.price;
        sum_ += t.price;
        head_ = (head_ + 1) % window;

        if (count_ < window) {
            ++count_;
            return false;
        }

        // Compute mean
        const double mean = sum_ / static_cast<double>(window);
        const double diff = t.price - mean;

        if (diff >= cfg_.deviation) {
            fill_intent(out, t, 1); // sell - price above mean
            return true;
        }

        if (diff <= -cfg_.deviation) {
            fill_intent(out, t, 0); // buy - price below mean
            return true;
        }

        return false;
    }

private:
    MeanReversionConfig cfg_;
    double prices_[MAX_WINDOW];
    double sum_;
    uint32_t head_;
    uint32_t count_;
};

} // namespace strategy
} // namespace chimera
