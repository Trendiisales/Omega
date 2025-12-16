#pragma once
#include <mutex>
#include <chrono>

namespace Omega {

class FIXQuoteThrottle {
public:
    FIXQuoteThrottle();

    void setMinGapMs(int ms);
    bool allow();

private:
    std::mutex lock;
    int gap=5;
    long last=0;
};

}
