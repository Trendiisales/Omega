#include "FIXDepthNormalizer.hpp"

namespace Omega {

void FIXDepthNormalizer::normalize(const std::vector<FIXMDEntry>& v,
                                   OrderBook& ob)
{
    ob.clear();
    for(auto& e : v){
        int lvl = (e.level>0?e.level-1:-1);
        if(lvl<0 || lvl>=10) continue;

        if(e.type==0){
            ob.bidPrice[lvl]=e.px;
            ob.bidSize[lvl]=e.qty;
        }
        else if(e.type==1){
            ob.askPrice[lvl]=e.px;
            ob.askSize[lvl]=e.qty;
        }
    }
}

}
