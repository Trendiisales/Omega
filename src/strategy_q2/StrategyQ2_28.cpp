#include "StrategyQ2_28.hpp"
#include <cmath>
namespace Omega {
StrategyQ2_28::StrategyQ2_28() : velocity(0), lastMid(0) {}
double StrategyQ2_28::compute(const Tick& t,const OrderBook& ob,const MicroState& ms,const double* base){
    double mid=0.5*(t.bid+t.ask);
    double d=mid-lastMid; lastMid=mid;
    velocity=0.9*velocity+0.1*d;
    double obTilt=0; double B=ob.bidSize[0]+ob.bidSize[4]; double A=ob.askSize[0]+ob.askSize[4];
    if(B+A>0) obTilt=(B-A)/(B+A);
    double fuse=(base[11]+base[19]+base[27])*0.3333;
    return velocity*0.40 + d*0.25 + obTilt*0.20 + ms.v[15]*0.10 + fuse*0.05;
}
}
