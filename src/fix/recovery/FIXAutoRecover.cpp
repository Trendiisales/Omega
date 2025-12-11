#include "FIXAutoRecover.hpp"
#include <chrono>

namespace Omega {

FIXAutoRecover::FIXAutoRecover(FIXSession* s,FIXMDSubscription* m,FIXReplayBuffer* r)
: sess(s), subs(m), replay(r), running(false) {}

FIXAutoRecover::~FIXAutoRecover(){
    stop();
}

void FIXAutoRecover::addSymbol(const std::string& s){
    std::lock_guard<std::mutex> g(lock);
    watch.insert(s);
}

void FIXAutoRecover::start(){
    running=true;
    th = std::thread(&FIXAutoRecover::loop,this);
}

void FIXAutoRecover::stop(){
    running=false;
    if(th.joinable()) th.join();
}

void FIXAutoRecover::loop(){
    using namespace std::chrono;

    while(running){
        std::this_thread::sleep_for(seconds(15));

        std::unordered_set<std::string> local;
        {
            std::lock_guard<std::mutex> g(lock);
            local = watch;
        }

        for(auto& s : local){
            recoverSymbol(s);
        }
    }
}

void FIXAutoRecover::recoverSymbol(const std::string& sym){
    FIXMessage snap;
    FIXMessage req;

    req.set(35,"V");
    req.set(263,"1");
    req.set(55,sym);
    sess->sendMessage(req);

    std::vector<FIXMessage> range;
    replay->getRange(1,9999999,range);
}

}
