#include "StrategyQ2_20.hpp"
#include <cmath>
namespace Omega {
StrategyQ2_20::StrategyQ2_20() : vol(0), volAvg(0) {}
double StrategyQ2_20::compute(const Tick& t,const OrderBook& ob,const MicroState& ms,const double* base){
    double v=t.buyVol+t.sellVol;
    volAvg=0.9*volAvg+0.1*v;
    double shock=v-volAvg;
    double ht=0; double B=ob.bidSize[4]; double A=ob.askSize[4];
    if(B+A>0) ht=(B-A)/(B+A);
    double fuse=(base[10]+base[18]+base[26])*0.3333;
    return shock*0.45 + ht*0.25 + ms.v[7]*0.20 + fuse*0.10;
}
}
