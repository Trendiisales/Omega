#include "StrategyQ2_26.hpp"
#include <cmath>
namespace Omega {
StrategyQ2_26::StrategyQ2_26() : drift(0), last(0) {}
double StrategyQ2_26::compute(const Tick& t,const OrderBook& ob,const MicroState& ms,const double* base){
    double px=0.5*(t.bid+t.ask);
    double d=px-last; last=px;
    drift=0.92*drift+0.08*d;
    double lvl=0; double B=ob.bidSize[1]+ob.bidSize[2]; double A=ob.askSize[1]+ob.askSize[2];
    if(B+A>0) lvl=(B-A)/(B+A);
    double fuse=(base[7]+base[14]+base[21])*0.3333;
    return drift*0.40 + d*0.25 + lvl*0.20 + ms.v[13]*0.10 + fuse*0.05;
}
}
