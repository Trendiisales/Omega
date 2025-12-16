// =============================================================================
// TickValidator.hpp - Tick Validity & Time Sanity Enforcement
// =============================================================================
// Hot-path: pure checks, no allocation, branch-only
// Cold-path: escalation via EngineHealth
// =============================================================================
#pragma once

#include <cstdint>
#include "market/MarketTypes.hpp"
#include "engine/EngineHealth.hpp"

namespace chimera {
namespace market {

struct TickValidator {
    uint64_t max_future_skew_ns;     // Max exchange ts can be ahead of ingress
    uint64_t max_backward_skew_ns;   // Max exchange ts can go backwards
    uint64_t max_freeze_ns;          // Max time without exchange ts update

    TickValidator() noexcept
        : max_future_skew_ns(5'000'000'000ULL)      // 5 seconds
        , max_backward_skew_ns(100'000'000ULL)      // 100ms
        , max_freeze_ns(1'000'000'000ULL)           // 1 second
    {}

    TickValidator(uint64_t future_ns, uint64_t backward_ns, uint64_t freeze_ns) noexcept
        : max_future_skew_ns(future_ns)
        , max_backward_skew_ns(backward_ns)
        , max_freeze_ns(freeze_ns)
    {}

    inline bool validate(const Tick& t,
                         uint64_t& last_ingress_ts,
                         uint64_t& last_exchange_ts,
                         uint64_t& last_exchange_update_ts,
                         chimera::engine::EngineHealth& health) const noexcept
    {
        // Flag consistency checks
        if ((t.flags & TICK_HAS_PRICE) && !(t.price > 0.0)) {
            health.invalid_ticks.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        if ((t.flags & TICK_HAS_SIZE) && !(t.size > 0.0)) {
            health.invalid_ticks.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        // Trade must have price AND size
        if ((t.flags & TICK_IS_TRADE) &&
            !((t.flags & TICK_HAS_PRICE) && (t.flags & TICK_HAS_SIZE))) {
            health.invalid_ticks.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        // Valid side
        if (t.side > SIDE_TRADE) {
            health.invalid_ticks.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        // Ingress time must be monotonic
        if (t.ingress_ts_ns < last_ingress_ts) {
            health.invalid_ticks.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        last_ingress_ts = t.ingress_ts_ns;

        // Exchange time backward check (with tolerance)
        if (last_exchange_ts > 0 && 
            t.exchange_ts_ns + max_backward_skew_ns < last_exchange_ts) {
            health.invalid_ticks.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        // Track exchange timestamp updates
        if (t.exchange_ts_ns > last_exchange_ts) {
            last_exchange_ts = t.exchange_ts_ns;
            last_exchange_update_ts = t.ingress_ts_ns;
        } else {
            // Check for frozen exchange timestamp
            if (last_exchange_update_ts > 0 &&
                t.ingress_ts_ns - last_exchange_update_ts > max_freeze_ns) {
                health.invalid_ticks.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
        }

        // Exchange time not wildly in future
        if (t.exchange_ts_ns > t.ingress_ts_ns + max_future_skew_ns) {
            health.invalid_ticks.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        return true;
    }

    // Simpler validation without time tracking (for testing)
    inline bool validate_basic(const Tick& t,
                               chimera::engine::EngineHealth& health) const noexcept
    {
        if ((t.flags & TICK_HAS_PRICE) && !(t.price > 0.0)) {
            health.invalid_ticks.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        if ((t.flags & TICK_HAS_SIZE) && !(t.size > 0.0)) {
            health.invalid_ticks.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        if ((t.flags & TICK_IS_TRADE) &&
            !((t.flags & TICK_HAS_PRICE) && (t.flags & TICK_HAS_SIZE))) {
            health.invalid_ticks.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        if (t.side > SIDE_TRADE) {
            health.invalid_ticks.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        return true;
    }
};

} // namespace market
} // namespace chimera
