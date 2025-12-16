#include "StrategyQ2_13.hpp"
#include <cmath>
namespace Omega {
StrategyQ2_13::StrategyQ2_13() : emaVol(0) {}
double StrategyQ2_13::compute(const Tick& t,const OrderBook& ob,const MicroState& ms,const double* base){
    double v=t.buyVol+t.sellVol;
    emaVol=0.9*emaVol+0.1*v;
    double shock=v-emaVol;
    double skew=0; double B=ob.bidSize[0]+ob.bidSize[3]; double A=ob.askSize[0]+ob.askSize[3];
    if(B+A>0) skew=(B-A)/(B+A);
    double fused=(base[0]+base[7]+base[14])*0.3333;
    return shock*0.45 + skew*0.30 + ms.v[14]*0.15 + fused*0.10;
}
}
