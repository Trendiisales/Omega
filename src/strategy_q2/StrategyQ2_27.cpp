#include "StrategyQ2_27.hpp"
#include <cmath>
namespace Omega {
StrategyQ2_27::StrategyQ2_27() : mom(0) {}
double StrategyQ2_27::compute(const Tick& t,const OrderBook& ob,const MicroState& ms,const double* base){
    double m=0.5*(t.bid+t.ask);
    mom=0.88*mom + 0.12*(m*0.0001);
    double imbalance=0; double B=ob.bidSize[3]; double A=ob.askSize[3];
    if(B+A>0) imbalance=(B-A)/(B+A);
    double fuse=(base[8]+base[9]+base[10])/3.0;
    return mom*0.40 + imbalance*0.25 + ms.v[14]*0.20 + fuse*0.10;
}
}
