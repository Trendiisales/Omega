#include "FIXFeedMux.hpp"

namespace Omega {

FIXFeedMux::FIXFeedMux(){}

void FIXFeedMux::setBridge(FIXBridge* b){
    bridge=b;
}

void FIXFeedMux::setMap(FIXSymbolMap* m){
    smap=m;
}

void FIXFeedMux::onFIX(const FIXMessage& msg){
    std::lock_guard<std::mutex> g(lock);
    if(!bridge) return;

    std::string sym = msg.get(55);
    if(smap) sym = smap->resolve(sym);

    FIXMessage m2 = msg;
    m2.fields[55] = sym;

    bridge->process(m2);
}

}
