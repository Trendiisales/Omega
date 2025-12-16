#include "ThreadPool.hpp"

namespace Omega {

ThreadPool::ThreadPool(size_t n)
    : running(true)
{
    for (size_t i = 0; i < n; ++i) {
        threads.emplace_back(&ThreadPool::worker, this, (int)i);
    }
}

ThreadPool::~ThreadPool() {
    stop();
}

void ThreadPool::enqueue(std::function<void()> job) {
    {
        std::lock_guard<std::mutex> lock(mtx);
        jobs.push(job);
    }
    cv.notify_one();
}

void ThreadPool::stop() {
    running = false;
    cv.notify_all();

    for (auto& t : threads) {
        if (t.joinable()) t.join();
    }
}

void ThreadPool::worker(int id) {
    while (running) {
        std::function<void()> job;

        {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait(lock, [&]{
                return !jobs.empty() || !running;
            });

            if (!running && jobs.empty()) return;

            job = jobs.front();
            jobs.pop();
        }

        job();
    }
}

} // namespace Omega
