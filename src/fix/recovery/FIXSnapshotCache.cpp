#include "FIXSnapshotCache.hpp"

namespace Omega {

FIXSnapshotCache::FIXSnapshotCache(){}

void FIXSnapshotCache::store(const std::string& sym,const FIXMessage& m){
    std::lock_guard<std::mutex> g(lock);
    map[sym]=m;
}

bool FIXSnapshotCache::get(const std::string& sym,FIXMessage& m){
    std::lock_guard<std::mutex> g(lock);
    auto it=map.find(sym);
    if(it==map.end()) return false;
    m = it->second;
    return true;
}

}
