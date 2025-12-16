#include "StrategyQ2_18.hpp"
#include <cmath>
namespace Omega {
StrategyQ2_18::StrategyQ2_18() : impulse(0) {}
double StrategyQ2_18::compute(const Tick& t,const OrderBook& ob,const MicroState& ms,const double* base){
    double mid=0.5*(t.bid+t.ask);
    impulse=0.9*impulse+0.1*(t.delta*mid);
    double obp=0; double B=ob.bidSize[3]+ob.bidSize[5]; double A=ob.askSize[3]+ob.askSize[5];
    if(B+A>0) obp=(B-A)/(B+A);
    double fuse=(base[7]+base[15]+base[23])*0.3333;
    return impulse*0.45 + obp*0.30 + ms.v[2]*0.15 + fuse*0.10;
}
}
