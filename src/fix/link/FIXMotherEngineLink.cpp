#include "FIXMotherEngineLink.hpp"

namespace Omega {

FIXMotherEngineLink::FIXMotherEngineLink(){}

void FIXMotherEngineLink::setMother(MotherEngine* m){
    mother=m;
}

void FIXMotherEngineLink::setBridge(FIXBridge* b){
    bridge=b;
}

void FIXMotherEngineLink::onTick(const std::string& sym,const Tick& t){
    std::lock_guard<std::mutex> g(lock);
    if(!mother) return;
    mother->onExternalTick(sym,t);
}

void FIXMotherEngineLink::onBook(const std::string& sym,const OrderBook& ob){
    std::lock_guard<std::mutex> g(lock);
    if(!mother) return;
    mother->onExternalBook(sym,ob);
}

void FIXMotherEngineLink::onExec(const ExecReport& r){
    std::lock_guard<std::mutex> g(lock);
    if(!mother) return;
    mother->onExternalExec(r);
}

void FIXMotherEngineLink::onReject(const FIXRejectInfo& r){
    std::lock_guard<std::mutex> g(lock);
    if(!mother) return;
    mother->onExternalReject(r);
}

}
