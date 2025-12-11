#include "StrategyQ2_29.hpp"
#include <cmath>
namespace Omega {
StrategyQ2_29::StrategyQ2_29() : accel(0), last(0) {}
double StrategyQ2_29::compute(const Tick& t,const OrderBook& ob,const MicroState& ms,const double* base){
    double m=0.5*(t.bid+t.ask);
    double v=m-last; last=m;
    accel=0.88*accel+0.12*(v*v);
    double lvl=0; double B=ob.bidSize[1]+ob.bidSize[3]; double A=ob.askSize[1]+ob.askSize[3];
    if(B+A>0) lvl=(B-A)/(B+A);
    double fuse=(base[12]+base[20]+base[28])*0.3333;
    return accel*0.40 + lvl*0.25 + ms.v[16]*0.20 + fuse*0.10;
}
}
