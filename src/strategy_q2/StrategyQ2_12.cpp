#include "StrategyQ2_12.hpp"
#include <cmath>
namespace Omega {
StrategyQ2_12::StrategyQ2_12() : accel(0), lastMid(0) {}
double StrategyQ2_12::compute(const Tick& t,const OrderBook& ob,const MicroState& ms,const double* base){
    double mid=0.5*(t.bid+t.ask);
    double d=mid-lastMid; lastMid=mid;
    accel=0.88*accel+0.12*d;
    double obI=0; double B=ob.bidSize[2]; double A=ob.askSize[2];
    if(B+A>0) obI=(B-A)/(B+A);
    double fuse=(base[29]+base[30]+base[31])/3.0;
    return accel*0.40 + d*0.20 + obI*0.20 + ms.v[13]*0.10 + fuse*0.10;
}
}
