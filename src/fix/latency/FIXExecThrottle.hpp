#pragma once
#include <mutex>
#include <chrono>

namespace Omega {

class FIXExecThrottle {
public:
    FIXExecThrottle();

    void setMinIntervalMs(int ms);
    bool allow();

private:
    std::mutex lock;
    int minMs=5;
    long last=0;
};

}
