#include "StrategyQ2_19.hpp"
#include <cmath>
namespace Omega {
StrategyQ2_19::StrategyQ2_19() : drift(0), lastMid(0) {}
double StrategyQ2_19::compute(const Tick& t,const OrderBook& ob,const MicroState& ms,const double* base){
    double m=0.5*(t.bid+t.ask);
    double d=m-lastMid; lastMid=m;
    drift=0.92*drift+0.08*d;
    double depth=0; double B=ob.bidSize[0]+ob.bidSize[1]; double A=ob.askSize[0]+ob.askSize[1];
    if(B+A>0) depth=(B-A)/(B+A);
    double fuse=(base[8]+base[16]+base[24])*0.3333;
    return drift*0.40 + d*0.25 + depth*0.20 + ms.v[6]*0.10 + fuse*0.05;
}
}
