// moodycamel::ConcurrentQueue - Lock-free MPMC queue
// Simplified header for HFT use
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <type_traits>
#include <algorithm>
#include <limits>
#include <utility>
#include <new>

namespace moodycamel {

template<typename T>
class ConcurrentQueue {
    static constexpr size_t BLOCK_SIZE = 32;
    static constexpr size_t CACHE_LINE = 64;
    
    struct Block {
        alignas(CACHE_LINE) std::atomic<size_t> front{0};
        alignas(CACHE_LINE) std::atomic<size_t> tail{0};
        alignas(CACHE_LINE) T data[BLOCK_SIZE];
        alignas(CACHE_LINE) std::atomic<bool> occupied[BLOCK_SIZE] = {};
        Block* next{nullptr};
    };
    
public:
    ConcurrentQueue() : head_(new Block()), tail_(head_.load()) {}
    
    ~ConcurrentQueue() {
        Block* b = head_.load();
        while (b) {
            Block* next = b->next;
            delete b;
            b = next;
        }
    }
    
    // Non-copyable
    ConcurrentQueue(const ConcurrentQueue&) = delete;
    ConcurrentQueue& operator=(const ConcurrentQueue&) = delete;
    
    inline bool enqueue(const T& item) {
        return inner_enqueue(item);
    }
    
    inline bool enqueue(T&& item) {
        return inner_enqueue(std::move(item));
    }
    
    inline bool try_dequeue(T& item) {
        Block* b = head_.load(std::memory_order_acquire);
        size_t front = b->front.load(std::memory_order_acquire);
        
        while (b) {
            size_t tail = b->tail.load(std::memory_order_acquire);
            
            if (front < tail || b->occupied[front % BLOCK_SIZE].load(std::memory_order_acquire)) {
                size_t idx = front % BLOCK_SIZE;
                if (b->occupied[idx].load(std::memory_order_acquire)) {
                    item = std::move(b->data[idx]);
                    b->occupied[idx].store(false, std::memory_order_release);
                    b->front.fetch_add(1, std::memory_order_acq_rel);
                    return true;
                }
            }
            
            // Try next block if this one is exhausted
            if (b->next) {
                b = b->next;
                front = b->front.load(std::memory_order_acquire);
            } else {
                break;
            }
        }
        return false;
    }
    
    inline size_t size_approx() const {
        size_t count = 0;
        Block* b = head_.load(std::memory_order_acquire);
        while (b) {
            size_t tail = b->tail.load(std::memory_order_acquire);
            size_t front = b->front.load(std::memory_order_acquire);
            if (tail > front) count += tail - front;
            b = b->next;
        }
        return count;
    }
    
private:
    template<typename U>
    inline bool inner_enqueue(U&& item) {
        Block* b = tail_.load(std::memory_order_acquire);
        size_t tail = b->tail.fetch_add(1, std::memory_order_acq_rel);
        size_t idx = tail % BLOCK_SIZE;
        
        if (tail >= BLOCK_SIZE) {
            // Need new block
            Block* newBlock = new Block();
            b->next = newBlock;
            tail_.store(newBlock, std::memory_order_release);
            b = newBlock;
            tail = b->tail.fetch_add(1, std::memory_order_acq_rel);
            idx = tail % BLOCK_SIZE;
        }
        
        b->data[idx] = std::forward<U>(item);
        b->occupied[idx].store(true, std::memory_order_release);
        return true;
    }
    
    alignas(CACHE_LINE) std::atomic<Block*> head_;
    alignas(CACHE_LINE) std::atomic<Block*> tail_;
};

} // namespace moodycamel
