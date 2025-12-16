#include "StrategyQ2_11.hpp"
#include <cmath>
namespace Omega {
StrategyQ2_11::StrategyQ2_11() : mom(0) {}
double StrategyQ2_11::compute(const Tick& t,const OrderBook& ob,const MicroState& ms,const double* base){
    double px=0.5*(t.bid+t.ask);
    mom=0.9*mom+0.1*(px*0.0001);
    double depth=0; double B=ob.bidSize[1]; double A=ob.askSize[1];
    if(B+A>0) depth=(B-A)/(B+A);
    double fuse=(base[27]+base[28])/2.0;
    return mom*0.45 + depth*0.25 + ms.v[12]*0.20 + fuse*0.10;
}
}
