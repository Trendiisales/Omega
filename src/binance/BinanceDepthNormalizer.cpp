#include "BinanceDepthNormalizer.hpp"
#include <algorithm>

namespace Omega {

void BinanceDepthNormalizer::toOrderBook(
            const std::vector<std::pair<double,double>>& bids,
            const std::vector<std::pair<double,double>>& asks,
            OrderBook& ob)
{
    for(int i=0;i<10;i++){
        ob.bidPrice[i]=0; ob.bidSize[i]=0;
        ob.askPrice[i]=0; ob.askSize[i]=0;
    }

    int bi=0;
    for(auto& x : bids) {
        if(bi>=10) break;
        ob.bidPrice[bi]=x.first;
        ob.bidSize [bi]=x.second;
        bi++;
    }

    int ai=0;
    for(auto& x : asks) {
        if(ai>=10) break;
        ob.askPrice[ai]=x.first;
        ob.askSize [ai]=x.second;
        ai++;
    }
}

}
