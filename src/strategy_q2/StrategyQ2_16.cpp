#include "StrategyQ2_16.hpp"
#include <cmath>
namespace Omega {
StrategyQ2_16::StrategyQ2_16() : accel(0), last(0) {}
double StrategyQ2_16::compute(const Tick& t,const OrderBook& ob,const MicroState& ms,const double* base){
    double px=0.5*(t.bid+t.ask);
    double d=px-last; last=px;
    accel=0.9*accel+0.1*(d*d);
    double tilt=0; double B=ob.bidSize[0]; double A=ob.askSize[0];
    if(B+A>0) tilt=(B-A)/(B+A);
    double fused=(base[4]+base[12]+base[20])*0.3333;
    return accel*0.45 + tilt*0.25 + ms.v[0]*0.20 + fused*0.10;
}
}
