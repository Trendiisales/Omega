// =============================================================================
// BacktestTickSource.hpp - Historical Replay Source
// =============================================================================
// Reads CanonicalTick from a preloaded array.
// Same validation, same queues, same kill behavior as live.
// =============================================================================
#pragma once

#include <cstddef>
#include "data/TickSource.hpp"

namespace chimera {
namespace data {

class BacktestTickSource final : public TickSource {
public:
    using Tick = chimera::market::Tick;

    BacktestTickSource(const Tick* ticks, std::size_t count) noexcept
        : ticks_(ticks)
        , count_(count)
        , idx_(0)
    {}

    bool next(Tick& out) noexcept override {
        if (idx_ >= count_) return false;
        out = ticks_[idx_++];
        return true;
    }

    void reset() noexcept override {
        idx_ = 0;
    }

    std::size_t size() const noexcept override {
        return count_;
    }

    std::size_t position() const noexcept {
        return idx_;
    }

private:
    const Tick* ticks_;
    std::size_t count_;
    std::size_t idx_;
};

} // namespace data
} // namespace chimera
