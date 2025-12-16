// =============================================================================
// ExecutionHealth.hpp - Execution-Side Health Tracking
// =============================================================================
// Tracks execution-side failures for FIX/WebSocket transport
// =============================================================================
#pragma once

#include <atomic>
#include <cstdint>

namespace chimera {
namespace engine {

struct alignas(64) ExecutionHealth {
    std::atomic<uint64_t> send_attempts;
    std::atomic<uint64_t> send_drops;
    std::atomic<uint64_t> send_successes;
    std::atomic<uint64_t> socket_errors;
    std::atomic<uint64_t> partial_writes;

    std::atomic<uint8_t> killed;

    ExecutionHealth() noexcept {
        reset();
    }

    inline void reset() noexcept {
        send_attempts.store(0, std::memory_order_relaxed);
        send_drops.store(0, std::memory_order_relaxed);
        send_successes.store(0, std::memory_order_relaxed);
        socket_errors.store(0, std::memory_order_relaxed);
        partial_writes.store(0, std::memory_order_relaxed);
        killed.store(0, std::memory_order_relaxed);
    }

    inline bool is_killed() const noexcept {
        return killed.load(std::memory_order_acquire) != 0;
    }

    inline void kill() noexcept {
        killed.store(1, std::memory_order_release);
    }

    // Hot path
    inline void record_attempt() noexcept {
        send_attempts.fetch_add(1, std::memory_order_relaxed);
    }

    inline void record_success() noexcept {
        send_successes.fetch_add(1, std::memory_order_relaxed);
    }

    inline void record_drop() noexcept {
        send_drops.fetch_add(1, std::memory_order_relaxed);
    }

    inline void record_error() noexcept {
        socket_errors.fetch_add(1, std::memory_order_relaxed);
    }

    inline void record_partial() noexcept {
        partial_writes.fetch_add(1, std::memory_order_relaxed);
    }
};

} // namespace engine
} // namespace chimera
