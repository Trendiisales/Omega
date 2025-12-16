// =============================================================================
// MonotonicClock.hpp - Monotonic Time Source
// =============================================================================
// HARD RULES:
// - Uses std::chrono::steady_clock (guaranteed monotonic)
// - NOT high_resolution_clock (may not be monotonic)
// - Nanosecond resolution
// - No syscalls beyond initial call
// =============================================================================
#pragma once

#include <cstdint>
#include <chrono>

namespace chimera {
namespace core {

class MonotonicClock {
public:
    // Get current monotonic timestamp in nanoseconds
    static inline uint64_t now_ns() noexcept {
        return static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count()
        );
    }

    // Get current monotonic timestamp in microseconds
    static inline uint64_t now_us() noexcept {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()
            ).count()
        );
    }

    // Get current monotonic timestamp in milliseconds
    static inline uint64_t now_ms() noexcept {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()
            ).count()
        );
    }
};

} // namespace core
} // namespace chimera
