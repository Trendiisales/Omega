#include "StrategyQ2_22.hpp"
#include <cmath>
namespace Omega {
StrategyQ2_22::StrategyQ2_22() : drift(0), lastMid(0) {}
double StrategyQ2_22::compute(const Tick& t,const OrderBook& ob,const MicroState& ms,const double* base){
    double m=0.5*(t.bid+t.ask);
    double d=m-lastMid; lastMid=m;
    drift=0.9*drift+0.1*d;
    double depth=0; double B=ob.bidSize[1]; double A=ob.askSize[1];
    if(B+A>0) depth=(B-A)/(B+A);
    double fuse=(base[3]+base[9]+base[15]) / 3.0;
    return drift*0.40 + d*0.25 + depth*0.20 + ms.v[9]*0.10 + fuse*0.05;
}
}
