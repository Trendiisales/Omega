#include "StrategyQ2_10.hpp"
#include <cmath>
namespace Omega {
StrategyQ2_10::StrategyQ2_10() : shock(0) {}
double StrategyQ2_10::compute(const Tick& t,const OrderBook& ob,const MicroState& ms,const double* base){
    double mid=0.5*(t.bid+t.ask);
    shock = 0.85*shock + 0.15*(t.buyVol+t.sellVol);
    double obp=0; double B=ob.bidSize[0]+ob.bidSize[4]; double A=ob.askSize[0]+ob.askSize[4];
    if(B+A>0) obp=(B-A)/(B+A);
    double fuse=(base[24]+base[25]+base[26])/3.0;
    return shock*0.40 + obp*0.30 + ms.v[11]*0.15 + fuse*0.15;
}
}
