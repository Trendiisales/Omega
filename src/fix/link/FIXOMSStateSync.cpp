#include "FIXOMSStateSync.hpp"

namespace Omega {

FIXOMSStateSync::FIXOMSStateSync(){}

void FIXOMSStateSync::update(const ExecReport& r){
    std::lock_guard<std::mutex> g(lock);
    state.update(r);

    if(r.status=="4"){ // cancelled
        pendingCancel.erase(r.clOrdId);
    }

    if(r.status=="0" && replaceMap.count(r.clOrdId)){
        std::string oldID = replaceMap[r.clOrdId];
        replaceMap.erase(r.clOrdId);
        pendingCancel.erase(oldID);
    }
}

void FIXOMSStateSync::markPendingCancel(const std::string& id){
    std::lock_guard<std::mutex> g(lock);
    pendingCancel[id]="1";
}

void FIXOMSStateSync::markReplace(const std::string& oldID,const std::string& newID){
    std::lock_guard<std::mutex> g(lock);
    replaceMap[newID] = oldID;
}

OrderStateRecord FIXOMSStateSync::get(const std::string& id){
    std::lock_guard<std::mutex> g(lock);
    return state.get(id);
}

}
