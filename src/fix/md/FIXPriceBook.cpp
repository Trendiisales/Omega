#include "FIXPriceBook.hpp"
#include <algorithm>
#include <cstring>

namespace Omega {

FIXPriceBook::FIXPriceBook() {
    std::lock_guard<std::mutex> g(lock);
    ob.clear();
}

void FIXPriceBook::applySnapshot(const std::vector<FIXMDEntry>& v){
    std::lock_guard<std::mutex> g(lock);
    ob.clear();

    // Map FIX entries → price levels
    for(auto& e : v){
        applyEntry(e);
    }
}

void FIXPriceBook::applyIncremental(const std::vector<FIXMDEntry>& v){
    std::lock_guard<std::mutex> g(lock);
    for(auto& e : v){
        applyEntry(e);
    }
}

void FIXPriceBook::applyEntry(const FIXMDEntry& e){
    int lvl = (e.level>0 ? e.level-1 : -1);

    if(e.type==0){ // bid
        if(lvl>=0 && lvl<10){
            ob.bidPrice[lvl] = e.px;
            ob.bidSize[lvl]  = e.qty;
        }
    }
    else if(e.type==1){ // ask
        if(lvl>=0 && lvl<10){
            ob.askPrice[lvl] = e.px;
            ob.askSize[lvl]  = e.qty;
        }
    }
}

OrderBook FIXPriceBook::get() const {
    std::lock_guard<std::mutex> g(lock);
    return ob;
}

}
