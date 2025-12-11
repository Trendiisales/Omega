#include "FIXExecThrottle.hpp"
#include <chrono>

namespace Omega {

FIXExecThrottle::FIXExecThrottle(){}

void FIXExecThrottle::setMinIntervalMs(int ms){
    std::lock_guard<std::mutex> g(lock);
    minMs = ms;
}

bool FIXExecThrottle::allow(){
    using namespace std::chrono;
    long now = duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();

    std::lock_guard<std::mutex> g(lock);
    if(now - last < minMs) return false;
    last = now;
    return true;
}

}
