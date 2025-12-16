#include "StrategyQ2_15.hpp"
#include <cmath>
namespace Omega {
StrategyQ2_15::StrategyQ2_15() : mom(0) {}
double StrategyQ2_15::compute(const Tick& t,const OrderBook& ob,const MicroState& ms,const double* base){
    double px=0.5*(t.bid+t.ask);
    mom=0.88*mom+0.12*(px*0.0001);
    double obp=0; double B=ob.bidSize[1]+ob.bidSize[4]; double A=ob.askSize[1]+ob.askSize[4];
    if(B+A>0) obp=(B-A)/(B+A);
    double f=(base[2]+base[9]+base[18])*0.3333;
    return mom*0.40 + obp*0.30 + ms.v[16]*0.20 + f*0.10;
}
}
