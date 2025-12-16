#include "FIXStabilityGuard.hpp"

namespace Omega {

FIXStabilityGuard::FIXStabilityGuard(){}

void FIXStabilityGuard::recordSpread(double s){
    std::lock_guard<std::mutex> g(lock);
    lastSpread=s;
}
void FIXStabilityGuard::recordVol(double v){
    std::lock_guard<std::mutex> g(lock);
    lastVol=v;
}

bool FIXStabilityGuard::stable(){
    std::lock_guard<std::mutex> g(lock);
    if(lastSpread>2.0) return false;
    if(lastVol>3.0) return false;
    return true;
}

}
