#include "FIXMultiSessionRouter.hpp"
#include <chrono>

namespace Omega {

FIXMultiSessionRouter::FIXMultiSessionRouter(){}

void FIXMultiSessionRouter::setPrimary(FIXSession* p){
    std::lock_guard<std::mutex> g(lock);
    primary=p;
}

void FIXMultiSessionRouter::setBackup(FIXSession* b){
    std::lock_guard<std::mutex> g(lock);
    backup=b;
}

void FIXMultiSessionRouter::setLatency(FIXLatencyMonitor* l){
    latency=l;
}

void FIXMultiSessionRouter::setThrottle(FIXExecThrottle* t){
    throttle=t;
}

bool FIXMultiSessionRouter::routeSend(FIXMessage& m){
    using namespace std::chrono;

    long now = duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();

    if(throttle && !throttle->allow()) return false;

    FIXSession* target=nullptr;

    {
        std::lock_guard<std::mutex> g(lock);
        target = useBackup? backup: primary;
    }

    if(!target) return false;

    if(latency) latency->recordSend(now);

    return target->sendMessage(m);
}

void FIXMultiSessionRouter::routeRecv(FIXMessage& m){
    using namespace std::chrono;
    long now = duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();

    if(latency) latency->recordRecv(now);

    if(m.get(35)=="0"){
        double p99 = latency->p99();
        if(p99>40){
            std::lock_guard<std::mutex> g(lock);
            useBackup = !useBackup;
        }
    }
}

}
