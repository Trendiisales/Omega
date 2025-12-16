// =============================================================================
// QueueMetrics.hpp - Queue Overflow Metrics
// =============================================================================
// Hot-path: atomic increments only
// Cold-path: windowed rate calculation
// =============================================================================
#pragma once

#include <atomic>
#include <cstdint>

namespace chimera {
namespace engine {

struct alignas(64) QueueMetrics {
    std::atomic<uint64_t> push_attempts;
    std::atomic<uint64_t> push_drops;
    std::atomic<uint64_t> last_window_attempts;
    std::atomic<uint64_t> last_window_drops;

    QueueMetrics() noexcept {
        reset();
    }

    inline void reset() noexcept {
        push_attempts.store(0, std::memory_order_relaxed);
        push_drops.store(0, std::memory_order_relaxed);
        last_window_attempts.store(0, std::memory_order_relaxed);
        last_window_drops.store(0, std::memory_order_relaxed);
    }

    // Hot path - called on every push
    inline void record_attempt() noexcept {
        push_attempts.fetch_add(1, std::memory_order_relaxed);
    }

    // Hot path - called on drop
    inline void record_drop() noexcept {
        push_drops.fetch_add(1, std::memory_order_relaxed);
    }

    // Cold path - get drop rate in PPM (parts per million)
    inline uint64_t get_window_drop_rate_ppm() noexcept {
        const uint64_t attempts = push_attempts.load(std::memory_order_relaxed);
        const uint64_t drops = push_drops.load(std::memory_order_relaxed);

        const uint64_t last_attempts = 
            last_window_attempts.exchange(attempts, std::memory_order_relaxed);
        const uint64_t last_drops = 
            last_window_drops.exchange(drops, std::memory_order_relaxed);

        const uint64_t delta_attempts = attempts - last_attempts;
        const uint64_t delta_drops = drops - last_drops;

        if (delta_attempts == 0) return 0;

        return (delta_drops * 1'000'000ULL) / delta_attempts;
    }
};

} // namespace engine
} // namespace chimera
