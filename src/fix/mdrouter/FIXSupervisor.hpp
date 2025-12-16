#pragma once
#include <thread>
#include <atomic>
#include <mutex>
#include "../FIXSession.hpp"
#include "../session/FIXResend.hpp"
#include "../session/FIXGapFill.hpp"

namespace Omega {

class FIXSupervisor {
public:
    FIXSupervisor(FIXSession*);
    ~FIXSupervisor();

    void start();
    void stop();

private:
    FIXSession* sess;
    std::thread th;
    std::atomic<bool> running;

    void loop();
};

}
