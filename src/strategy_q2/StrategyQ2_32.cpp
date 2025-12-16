#include "StrategyQ2_32.hpp"
#include <cmath>
namespace Omega {
StrategyQ2_32::StrategyQ2_32() : smooth(0), lastMid(0) {}
double StrategyQ2_32::compute(const Tick& t,const OrderBook& ob,const MicroState& ms,const double* base){
    double m=0.5*(t.bid+t.ask);
    double d=m-lastMid; lastMid=m;
    smooth=0.9*smooth+0.1*d;
    double depth=0; double B=ob.bidSize[3]; double A=ob.askSize[3];
    if(B+A>0) depth=(B-A)/(B+A);
    double fuse=(base[0]+base[5]+base[11])*0.3333;
    return smooth*0.40 + d*0.25 + depth*0.20 + ms.v[2]*0.10 + fuse*0.05;
}
}
