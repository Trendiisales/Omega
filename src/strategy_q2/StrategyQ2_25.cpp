#include "StrategyQ2_25.hpp"
#include <cmath>
namespace Omega {
StrategyQ2_25::StrategyQ2_25() : vol(0), avg(0) {}
double StrategyQ2_25::compute(const Tick& t,const OrderBook& ob,const MicroState& ms,const double* base){
    double v=t.buyVol+t.sellVol;
    avg=0.9*avg+0.1*v;
    double shock=v-avg;
    double imbalance=0; double B=ob.bidSize[4]; double A=ob.askSize[4];
    if(B+A>0) imbalance=(B-A)/(B+A);
    double fuse=(base[6]+base[16]+base[17])*0.3333;
    return shock*0.45 + imbalance*0.25 + ms.v[12]*0.20 + fuse*0.10;
}
}
