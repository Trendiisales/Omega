// =============================================================================
// LiveTickSource.hpp - Live Feed Adapter
// =============================================================================
// Wraps an EngineIngress for symmetry in wiring.
// =============================================================================
#pragma once

#include "data/TickSource.hpp"
#include "engine/EngineIngress.hpp"

namespace chimera {
namespace data {

template <std::size_t Q>
class LiveTickSource final : public TickSource {
public:
    using Tick = chimera::market::Tick;

    explicit LiveTickSource(chimera::engine::EngineIngress<Q>& ingress) noexcept
        : ingress_(ingress) {}

    bool next(Tick& out) noexcept override {
        return ingress_.pop_tick(out);
    }

private:
    chimera::engine::EngineIngress<Q>& ingress_;
};

} // namespace data
} // namespace chimera
