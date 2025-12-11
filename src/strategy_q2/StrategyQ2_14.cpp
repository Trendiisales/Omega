#include "StrategyQ2_14.hpp"
#include <cmath>
namespace Omega {
StrategyQ2_14::StrategyQ2_14() : drift(0), lastMid(0) {}
double StrategyQ2_14::compute(const Tick& t,const OrderBook& ob,const MicroState& ms,const double* base){
    double m=0.5*(t.bid+t.ask);
    double d=m-lastMid; lastMid=m;
    drift=0.93*drift+0.07*d;
    double obTilt=0; double B=ob.bidSize[2]; double A=ob.askSize[2];
    if(B+A>0) obTilt=(B-A)/(B+A);
    double fuse=(base[3]+base[11]+base[19])*0.3333;
    return drift*0.40 + d*0.25 + obTilt*0.20 + ms.v[15]*0.10 + fuse*0.05;
}
}
