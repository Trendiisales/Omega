#pragma once
#include <set>
#include <unordered_set>
#include <mutex>
#include <string>
#include <thread>
#include <atomic>
#include "../FIXSession.hpp"
#include "../mdrouter/FIXMDSubscription.hpp"

namespace Omega {

class FIXFeedLifecycle {
public:
    FIXFeedLifecycle(FIXSession*,FIXMDSubscription*);
    ~FIXFeedLifecycle();

    void start();
    void stop();

    void add(const std::string& sym);
    void remove(const std::string& sym);

private:
    FIXSession* sess;
    FIXMDSubscription* sub;

    std::thread th;
    std::atomic<bool> running;
    std::mutex lock;

    std::unordered_set<std::string> watch;

    void loop();
};

}
