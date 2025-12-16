#include "StrategyQ2_24.hpp"
#include <cmath>
namespace Omega {
StrategyQ2_24::StrategyQ2_24() : lastSpread(0) {}
double StrategyQ2_24::compute(const Tick& t,const OrderBook& ob,const MicroState& ms,const double* base){
    double d=t.spread-lastSpread; lastSpread=t.spread;
    double obTilt=0; double B=ob.bidSize[3]; double A=ob.askSize[3];
    if(B+A>0) obTilt=(B-A)/(B+A);
    double fuse=(base[5]+base[13]+base[21])/3.0;
    return d*0.45 + obTilt*0.30 + ms.v[11]*0.15 + fuse*0.10;
}
}
