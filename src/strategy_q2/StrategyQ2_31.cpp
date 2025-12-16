#include "StrategyQ2_31.hpp"
#include <cmath>
namespace Omega {
StrategyQ2_31::StrategyQ2_31() : momentum(0) {}
double StrategyQ2_31::compute(const Tick& t,const OrderBook& ob,const MicroState& ms,const double* base){
    double px=0.5*(t.bid+t.ask);
    momentum=0.9*momentum+0.1*(px*0.001);
    double tilt=0; double B=ob.bidSize[0]; double A=ob.askSize[0];
    if(B+A>0) tilt=(B-A)/(B+A);
    double fuse=(base[14]+base[22]+base[30])*0.3333;
    return momentum*0.40 + tilt*0.25 + ms.v[1]*0.20 + fuse*0.10;
}
}
