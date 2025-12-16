// =============================================================================
// LatencyStats.hpp - HFT-Safe Latency Statistics
// =============================================================================
// HOT PATH:
//   - thread-local increments only
//   - no atomics
//   - no fences
//
// COLD PATH:
//   - periodic aggregation
//   - atomics allowed
//
// Units: nanoseconds
// =============================================================================
#pragma once

#include <cstdint>
#include <cstddef>
#include <atomic>

namespace chimera {
namespace core {

// =============================================================================
// LatencyBucket - single measurement category
// =============================================================================
struct alignas(64) LatencyBucket {
    uint64_t count;
    uint64_t sum_ns;
    uint64_t min_ns;
    uint64_t max_ns;

    inline void reset() noexcept {
        count  = 0;
        sum_ns = 0;
        min_ns = UINT64_MAX;
        max_ns = 0;
    }

    inline void add(uint64_t ns) noexcept {
        ++count;
        sum_ns += ns;
        if (ns < min_ns) min_ns = ns;
        if (ns > max_ns) max_ns = ns;
    }

    inline uint64_t avg_ns() const noexcept {
        return (count > 0) ? (sum_ns / count) : 0;
    }
};

static_assert(sizeof(LatencyBucket) <= 64, "LatencyBucket must fit in cache line");

// =============================================================================
// ThreadLatencyStats - one instance PER THREAD
// =============================================================================
struct alignas(64) ThreadLatencyStats {
    LatencyBucket tick_to_signal;   // Tick ingress → signal computed
    LatencyBucket signal_to_intent; // Signal → intent generated
    LatencyBucket intent_to_exec;   // Intent → execution sent

    inline void reset() noexcept {
        tick_to_signal.reset();
        signal_to_intent.reset();
        intent_to_exec.reset();
    }
};

// =============================================================================
// GlobalLatencyStats - written ONLY by cold thread
// =============================================================================
struct alignas(64) GlobalLatencyStats {
    // Tick to signal
    std::atomic<uint64_t> tick_signal_count;
    std::atomic<uint64_t> tick_signal_sum_ns;
    std::atomic<uint64_t> tick_signal_min_ns;
    std::atomic<uint64_t> tick_signal_max_ns;

    // Signal to intent
    std::atomic<uint64_t> signal_intent_count;
    std::atomic<uint64_t> signal_intent_sum_ns;
    std::atomic<uint64_t> signal_intent_min_ns;
    std::atomic<uint64_t> signal_intent_max_ns;

    // Intent to exec
    std::atomic<uint64_t> intent_exec_count;
    std::atomic<uint64_t> intent_exec_sum_ns;
    std::atomic<uint64_t> intent_exec_min_ns;
    std::atomic<uint64_t> intent_exec_max_ns;

    GlobalLatencyStats() noexcept {
        reset();
    }

    void reset() noexcept {
        tick_signal_count.store(0, std::memory_order_relaxed);
        tick_signal_sum_ns.store(0, std::memory_order_relaxed);
        tick_signal_min_ns.store(UINT64_MAX, std::memory_order_relaxed);
        tick_signal_max_ns.store(0, std::memory_order_relaxed);

        signal_intent_count.store(0, std::memory_order_relaxed);
        signal_intent_sum_ns.store(0, std::memory_order_relaxed);
        signal_intent_min_ns.store(UINT64_MAX, std::memory_order_relaxed);
        signal_intent_max_ns.store(0, std::memory_order_relaxed);

        intent_exec_count.store(0, std::memory_order_relaxed);
        intent_exec_sum_ns.store(0, std::memory_order_relaxed);
        intent_exec_min_ns.store(UINT64_MAX, std::memory_order_relaxed);
        intent_exec_max_ns.store(0, std::memory_order_relaxed);
    }
};

// =============================================================================
// Cold-path aggregation - called periodically by supervisor thread
// =============================================================================
inline void aggregate_latency(const ThreadLatencyStats& local,
                              GlobalLatencyStats& global) noexcept
{
    // Tick to signal
    if (local.tick_to_signal.count > 0) {
        global.tick_signal_count.fetch_add(local.tick_to_signal.count, std::memory_order_relaxed);
        global.tick_signal_sum_ns.fetch_add(local.tick_to_signal.sum_ns, std::memory_order_relaxed);

        uint64_t cur_min = global.tick_signal_min_ns.load(std::memory_order_relaxed);
        if (local.tick_to_signal.min_ns < cur_min) {
            global.tick_signal_min_ns.store(local.tick_to_signal.min_ns, std::memory_order_relaxed);
        }

        uint64_t cur_max = global.tick_signal_max_ns.load(std::memory_order_relaxed);
        if (local.tick_to_signal.max_ns > cur_max) {
            global.tick_signal_max_ns.store(local.tick_to_signal.max_ns, std::memory_order_relaxed);
        }
    }

    // Signal to intent
    if (local.signal_to_intent.count > 0) {
        global.signal_intent_count.fetch_add(local.signal_to_intent.count, std::memory_order_relaxed);
        global.signal_intent_sum_ns.fetch_add(local.signal_to_intent.sum_ns, std::memory_order_relaxed);

        uint64_t cur_min = global.signal_intent_min_ns.load(std::memory_order_relaxed);
        if (local.signal_to_intent.min_ns < cur_min) {
            global.signal_intent_min_ns.store(local.signal_to_intent.min_ns, std::memory_order_relaxed);
        }

        uint64_t cur_max = global.signal_intent_max_ns.load(std::memory_order_relaxed);
        if (local.signal_to_intent.max_ns > cur_max) {
            global.signal_intent_max_ns.store(local.signal_to_intent.max_ns, std::memory_order_relaxed);
        }
    }

    // Intent to exec
    if (local.intent_to_exec.count > 0) {
        global.intent_exec_count.fetch_add(local.intent_to_exec.count, std::memory_order_relaxed);
        global.intent_exec_sum_ns.fetch_add(local.intent_to_exec.sum_ns, std::memory_order_relaxed);

        uint64_t cur_min = global.intent_exec_min_ns.load(std::memory_order_relaxed);
        if (local.intent_to_exec.min_ns < cur_min) {
            global.intent_exec_min_ns.store(local.intent_to_exec.min_ns, std::memory_order_relaxed);
        }

        uint64_t cur_max = global.intent_exec_max_ns.load(std::memory_order_relaxed);
        if (local.intent_to_exec.max_ns > cur_max) {
            global.intent_exec_max_ns.store(local.intent_to_exec.max_ns, std::memory_order_relaxed);
        }
    }
}

} // namespace core
} // namespace chimera
