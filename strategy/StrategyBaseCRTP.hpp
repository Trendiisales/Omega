// =============================================================================
// StrategyBaseCRTP.hpp - CRTP Base for All Strategies
// =============================================================================
// HARD RULES:
// - No virtuals
// - Deterministic per-tick cost
// - Derived must implement: bool on_tick_impl(const Tick&, Intent&)
// =============================================================================
#pragma once

#include <cstdint>
#include "market/MarketTypes.hpp"
#include "engine/Intent.hpp"

namespace chimera {
namespace strategy {

// =============================================================================
// StrategyBaseCRTP
// =============================================================================
template <typename Derived>
class StrategyBaseCRTP {
public:
    using Tick   = chimera::market::Tick;
    using Intent = chimera::engine::Intent;

    inline bool on_tick(const Tick& t, Intent& out) noexcept {
        return static_cast<Derived*>(this)->on_tick_impl(t, out);
    }

protected:
    // Helper to populate intent
    static inline void fill_intent(Intent& out,
                                   const Tick& t,
                                   uint8_t side) noexcept {
        out.symbol_id = t.symbol_id;
        out.side      = side;
        out.venue     = t.venue;
        out.price     = t.price;
        out.size      = t.size;
        out.ts_ns     = 0; // Will be set by engine
    }
};

} // namespace strategy
} // namespace chimera
