#pragma once
#include <thread>
#include <atomic>
#include <mutex>
#include <unordered_set>
#include "../FIXSession.hpp"
#include "../mdrouter/FIXMDSubscription.hpp"
#include "FIXReplayBuffer.hpp"

namespace Omega {

class FIXAutoRecover {
public:
    FIXAutoRecover(FIXSession*,FIXMDSubscription*,FIXReplayBuffer*);
    ~FIXAutoRecover();

    void start();
    void stop();

    void addSymbol(const std::string&);

private:
    FIXSession* sess;
    FIXMDSubscription* subs;
    FIXReplayBuffer* replay;

    std::unordered_set<std::string> watch;
    std::mutex lock;

    std::thread th;
    std::atomic<bool> running;

    void loop();
    void recoverSymbol(const std::string&);
};

}
