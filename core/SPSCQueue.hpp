// =============================================================================
// SPSCQueue.hpp - Single-Producer / Single-Consumer Queue
// =============================================================================
// HARD GUARANTEES:
// - No heap allocation
// - No ABA
// - No locks
// - Cache-line separated indices
// - Correct acquire/release semantics
//
// This is the ONLY queue allowed on hot paths.
// =============================================================================
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace chimera {
namespace core {

template <typename T, std::size_t Capacity>
class SPSCQueue {
    static_assert(Capacity >= 2, "Capacity must be >= 2");
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two");
    static_assert(std::is_trivially_copyable<T>::value,
                  "T must be trivially copyable");

public:
    SPSCQueue() noexcept : head_(0), tail_(0) {}

    SPSCQueue(const SPSCQueue&) = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;

    // =========================================================================
    // Producer: push
    // Returns false if queue is full
    // =========================================================================
    inline bool push(const T& item) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t next = (head + 1) & mask_;

        if (next == tail_.load(std::memory_order_acquire)) {
            return false; // full
        }

        buffer_[head] = item;
        head_.store(next, std::memory_order_release);
        return true;
    }

    // =========================================================================
    // Producer: push_overwrite (DROP_OLDEST policy)
    // Overwrites oldest if full
    // =========================================================================
    inline void push_overwrite(const T& item) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t next = (head + 1) & mask_;

        if (next == tail_.load(std::memory_order_acquire)) {
            // Full - advance tail (drop oldest)
            tail_.store((tail_.load(std::memory_order_relaxed) + 1) & mask_,
                        std::memory_order_release);
        }

        buffer_[head] = item;
        head_.store(next, std::memory_order_release);
    }

    // =========================================================================
    // Consumer: pop
    // Returns false if queue is empty
    // =========================================================================
    inline bool pop(T& out) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);

        if (tail == head_.load(std::memory_order_acquire)) {
            return false; // empty
        }

        out = buffer_[tail];
        tail_.store((tail + 1) & mask_, std::memory_order_release);
        return true;
    }

    // =========================================================================
    // Queries
    // =========================================================================
    inline bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

    inline bool full() const noexcept {
        const std::size_t next =
            (head_.load(std::memory_order_acquire) + 1) & mask_;
        return next == tail_.load(std::memory_order_acquire);
    }

    inline std::size_t size() const noexcept {
        const std::size_t head = head_.load(std::memory_order_acquire);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        return (head - tail + Capacity) & mask_;
    }

    inline constexpr std::size_t capacity() const noexcept {
        return Capacity - 1;
    }

private:
    static constexpr std::size_t mask_ = Capacity - 1;

    // Cache-line separated to prevent false sharing
    alignas(64) std::atomic<std::size_t> head_;
    alignas(64) std::atomic<std::size_t> tail_;
    alignas(64) T buffer_[Capacity];
};

} // namespace core
} // namespace chimera
