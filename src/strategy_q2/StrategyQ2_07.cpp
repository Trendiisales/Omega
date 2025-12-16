#include "StrategyQ2_07.hpp"
#include <cmath>
namespace Omega {
StrategyQ2_07::StrategyQ2_07() : ema(0) {}
double StrategyQ2_07::compute(const Tick& t,const OrderBook& ob,const MicroState& ms,const double* base){
    double px=0.5*(t.bid+t.ask);
    ema=0.9*ema+0.1*px;
    double dev=px-ema;
    double depth=0;
    double B=ob.bidSize[1]+ob.bidSize[2];
    double A=ob.askSize[1]+ob.askSize[2];
    if(B+A>0) depth=(B-A)/(B+A);
    double fuse=(base[16]+base[17]+base[18])/3.0;
    return dev*0.40 + depth*0.30 + ms.v[8]*0.15 + fuse*0.15;
}
}
