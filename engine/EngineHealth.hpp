// =============================================================================
// EngineHealth.hpp - Per-Engine Health State
// =============================================================================
// Hot-path safe: atomic increments + loads only.
// Cold-path: kill decisions
// =============================================================================
#pragma once

#include <atomic>
#include <cstdint>

namespace chimera {
namespace engine {

enum class EngineKillReason : uint8_t {
    NONE = 0,
    TICK_QUEUE_OVERFLOW,
    INTENT_QUEUE_OVERFLOW,
    INVALID_TICK,
    EXECUTION_BACKPRESSURE,
    TIME_SANITY_FAILURE,
    MANUAL
};

struct alignas(64) EngineHealth {
    std::atomic<uint64_t> tick_drops;
    std::atomic<uint64_t> intent_drops;
    std::atomic<uint64_t> invalid_ticks;
    std::atomic<uint64_t> ticks_processed;

    std::atomic<uint8_t>  killed;
    std::atomic<uint8_t>  kill_reason;

    EngineHealth() noexcept {
        reset();
    }

    inline void reset() noexcept {
        tick_drops.store(0, std::memory_order_relaxed);
        intent_drops.store(0, std::memory_order_relaxed);
        invalid_ticks.store(0, std::memory_order_relaxed);
        ticks_processed.store(0, std::memory_order_relaxed);
        killed.store(0, std::memory_order_relaxed);
        kill_reason.store(
            static_cast<uint8_t>(EngineKillReason::NONE),
            std::memory_order_relaxed
        );
    }

    inline bool is_killed() const noexcept {
        return killed.load(std::memory_order_acquire) != 0;
    }

    inline void kill(EngineKillReason r) noexcept {
        kill_reason.store(
            static_cast<uint8_t>(r),
            std::memory_order_release
        );
        killed.store(1, std::memory_order_release);
    }

    inline EngineKillReason get_kill_reason() const noexcept {
        return static_cast<EngineKillReason>(
            kill_reason.load(std::memory_order_acquire)
        );
    }
};

} // namespace engine
} // namespace chimera
