// include/core/lock_free_queue.h
#pragma once
#include "../third_party/concurrentqueue/concurrentqueue.h"

template<typename T>
class LockFreeQueue {
public:
    inline void enqueue(const T& item) {
        m_queue.enqueue(item);
    }

    inline bool try_dequeue(T& item) {
        return m_queue.try_dequeue(item);
    }

private:
    moodycamel::ConcurrentQueue<T> m_queue;
};
