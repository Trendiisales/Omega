#include "UnifiedRecord.hpp"
#include <sstream>

namespace Omega {

void UnifiedRecord::syncTS(){
    ts = t.ts ? t.ts : ob.ts ? ob.ts : m.ts;
}

std::string UnifiedRecord::header(){
    return
    "ts,"
    "bid,ask,spread,buyVol,sellVol,liqGap,delta,b1,b2,a1,a2,"
    "ob_b1p,ob_b1s,ob_b2p,ob_b2s,ob_b3p,ob_b3s,ob_b4p,ob_b4s,ob_b5p,ob_b5s,"
    "ob_b6p,ob_b6s,ob_b7p,ob_b7s,ob_b8p,ob_b8s,ob_b9p,ob_b9s,ob_b10p,ob_b10s,"
    "ob_a1p,ob_a1s,ob_a2p,ob_a2s,ob_a3p,ob_a3s,ob_a4p,ob_a4s,ob_a5p,ob_a5s,"
    "ob_a6p,ob_a6s,ob_a7p,ob_a7s,ob_a8p,ob_a8s,ob_a9p,ob_a9s,ob_a10p,ob_a10s,"
    "mid,ofi,vpin,imbalance,shock,volburst,flow,regime,depthRatio";
}

std::string UnifiedRecord::encode(const UnifiedRecord& u){
    std::ostringstream o;

    o<<u.ts<<","<<u.t.bid<<","<<u.t.ask<<","<<u.t.spread<<","<<u.t.buyVol<<","<<u.t.sellVol<<","
     <<u.t.liquidityGap<<","<<u.t.delta<<","<<u.t.b1<<","<<u.t.b2<<","<<u.t.a1<<","<<u.t.a2;

    for(int i=0;i<10;i++)
        o<<","<<u.ob.bidPrice[i]<<","<<u.ob.bidSize[i];

    for(int i=0;i<10;i++)
        o<<","<<u.ob.askPrice[i]<<","<<u.ob.askSize[i];

    o<<","<<u.m.mid<<","<<u.m.ofi<<","<<u.m.vpin<<","<<u.m.imbalance<<","<<u.m.shock<<","<<u.m.volBurst
     <<","<<u.m.flow<<","<<u.m.regime<<","<<u.m.depthRatio;

    return o.str();
}

}
