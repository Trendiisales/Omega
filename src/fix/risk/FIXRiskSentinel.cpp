#include "FIXRiskSentinel.hpp"
#include <cstdlib>

namespace Omega {

FIXRiskSentinel::FIXRiskSentinel(){}

void FIXRiskSentinel::setMaxQty(double q){
    std::lock_guard<std::mutex> g(lock);
    maxQty=q;
}
void FIXRiskSentinel::setMaxNotional(double n){
    std::lock_guard<std::mutex> g(lock);
    maxNotional=n;
}

bool FIXRiskSentinel::check(const FIXMessage& m){
    double px = atof(m.get(44).c_str());
    double qty= atof(m.get(38).c_str());
    if(qty>maxQty) return false;
    if(px*qty>maxNotional) return false;
    return true;
}

}
