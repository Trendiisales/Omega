#include "StrategyQ2_08.hpp"
#include <cmath>
namespace Omega {
StrategyQ2_08::StrategyQ2_08() : trend(0), lastMid(0) {}
double StrategyQ2_08::compute(const Tick& t,const OrderBook& ob,const MicroState& ms,const double* base){
    double m=0.5*(t.bid+t.ask);
    double d=m-lastMid; lastMid=m;
    trend=0.92*trend+0.08*d;
    double im=0;
    double B=ob.bidSize[0]+ob.bidSize[2];
    double A=ob.askSize[0]+ob.askSize[2];
    if(B+A>0) im=(B-A)/(B+A);
    double fuse=(base[19]+base[20])/2.0;
    return trend*0.45 + d*0.25 + im*0.20 + ms.v[9]*0.05 + fuse*0.05;
}
}
