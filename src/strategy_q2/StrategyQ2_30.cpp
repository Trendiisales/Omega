#include "StrategyQ2_30.hpp"
#include <cmath>
namespace Omega {
StrategyQ2_30::StrategyQ2_30() : var(0) {}
double StrategyQ2_30::compute(const Tick& t,const OrderBook& ob,const MicroState& ms,const double* base){
    double mid=0.5*(t.bid+t.ask);
    var=0.96*var+0.04*(mid*mid);
    double vol=std::sqrt(std::max(0.0,var-mid*mid));
    double depth=0; double B=ob.bidSize[2]; double A=ob.askSize[2];
    if(B+A>0) depth=(B-A)/(B+A);
    double fuse=(base[13]+base[21]+base[29])*0.3333;
    return vol*0.40 + depth*0.25 + ms.v[0]*0.20 + fuse*0.10;
}
}
