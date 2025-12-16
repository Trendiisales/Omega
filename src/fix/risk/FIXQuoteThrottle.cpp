#include "FIXQuoteThrottle.hpp"
#include <chrono>

namespace Omega {

FIXQuoteThrottle::FIXQuoteThrottle(){}

void FIXQuoteThrottle::setMinGapMs(int ms){
    std::lock_guard<std::mutex> g(lock);
    gap=ms;
}

bool FIXQuoteThrottle::allow(){
    using namespace std::chrono;
    long now = duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();
    std::lock_guard<std::mutex> g(lock);
    if(now-last<gap) return false;
    last=now;
    return true;
}

}
