#include "StrategyQ2_23.hpp"
#include <cmath>
namespace Omega {
StrategyQ2_23::StrategyQ2_23() : impulse(0) {}
double StrategyQ2_23::compute(const Tick& t,const OrderBook& ob,const MicroState& ms,const double* base){
    double m=0.5*(t.bid+t.ask);
    impulse = 0.9*impulse + 0.1*(m*t.delta);
    double obp=0; double B=ob.bidSize[2]; double A=ob.askSize[2];
    if(B+A>0) obp=(B-A)/(B+A);
    double fuse=(base[4]+base[12]+base[20])*0.3333;
    return impulse*0.45 + obp*0.30 + ms.v[10]*0.15 + fuse*0.10;
}
}
