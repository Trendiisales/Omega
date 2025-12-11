#include "FIXMarketRouter.hpp"

namespace Omega {

FIXMarketRouter::FIXMarketRouter(){}

void FIXMarketRouter::setTickCallback(std::function<void(const std::string&,const Tick&)> cb){
    tcb=cb;
}
void FIXMarketRouter::setBookCallback(std::function<void(const std::string&,const OrderBook&)> cb){
    bcb=cb;
}

void FIXMarketRouter::update(const std::string& sym,
                             const std::vector<FIXMDEntry>& entries,
                             const FIXMDEntry& tobB,
                             const FIXMDEntry& tobA,
                             bool hasTOB)
{
    std::lock_guard<std::mutex> g(lock);
    auto& bw = bookMap[sym];

    if(!entries.empty()){
        bw.ob.clear();
        for(auto& e: entries){
            int lvl = (e.level>0?e.level-1:-1);
            if(lvl>=0 && lvl<10){
                if(e.type==0){
                    bw.ob.bidPrice[lvl] = e.px;
                    bw.ob.bidSize[lvl]  = e.qty;
                }
                if(e.type==1){
                    bw.ob.askPrice[lvl] = e.px;
                    bw.ob.askSize[lvl]  = e.qty;
                }
            }
        }
        bw.ob.ts = 0;
        if(bcb) bcb(sym,bw.ob);
    }

    if(hasTOB){
        Tick t{};
        t.symbol = sym;
        t.bid = tobB.px;
        t.ask = tobA.px;
        t.spread = t.ask - t.bid;
        t.ts = 0;
        if(tcb) tcb(sym,t);
    }
}

}
