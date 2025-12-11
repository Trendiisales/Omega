#pragma once
#include <vector>
#include <thread>
#include <queue>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <atomic>

namespace Omega {

class ThreadPool {
public:
    ThreadPool(size_t n = 4);
    ~ThreadPool();

    void enqueue(std::function<void()> job);

    void stop();

private:
    void worker(int id);

private:
    std::vector<std::thread> threads;
    std::queue<std::function<void()>> jobs;

    std::mutex mtx;
    std::condition_variable cv;

    std::atomic<bool> running;
};

} // namespace Omega
