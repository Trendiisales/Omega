#include "StrategyQ2_09.hpp"
#include <cmath>
namespace Omega {
StrategyQ2_09::StrategyQ2_09() : drift(0), last(0) {}
double StrategyQ2_09::compute(const Tick& t,const OrderBook& ob,const MicroState& ms,const double* base){
    double mid=0.5*(t.bid+t.ask);
    double d=mid-last; last=mid;
    drift=0.93*drift+0.07*d;
    double tilt=0; double b=ob.bidSize[3]; double a=ob.askSize[3];
    if(b+a>0) tilt=(b-a)/(b+a);
    double fuse=(base[21]+base[22]+base[23])/3.0;
    return drift*0.35 + d*0.25 + tilt*0.20 + ms.v[10]*0.10 + fuse*0.10;
}
}
