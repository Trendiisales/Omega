#include "FIXSymbolMap.hpp"

namespace Omega {

FIXSymbolMap::FIXSymbolMap(){}

void FIXSymbolMap::add(const std::string& a,const std::string& b){
    std::lock_guard<std::mutex> g(lock);
    map[a]=b;
}

std::string FIXSymbolMap::resolve(const std::string& a){
    std::lock_guard<std::mutex> g(lock);
    auto it = map.find(a);
    return it==map.end()?a:it->second;
}

}
