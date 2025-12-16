#include "StrategyQ2_21.hpp"
#include <cmath>
namespace Omega {
StrategyQ2_21::StrategyQ2_21() : ema(0) {}
double StrategyQ2_21::compute(const Tick& t,const OrderBook& ob,const MicroState& ms,const double* base){
    double px=0.5*(t.bid+t.ask);
    ema=0.93*ema+0.07*px;
    double dev=px-ema;
    double tilt=0; double B=ob.bidSize[0]; double A=ob.askSize[0];
    if(B+A>0) tilt=(B-A)/(B+A);
    double f=(base[1]+base[11])*0.5;
    return dev*0.45 + tilt*0.30 + ms.v[8]*0.15 + f*0.10;
}
}
