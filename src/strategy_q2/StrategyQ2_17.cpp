#include "StrategyQ2_17.hpp"
#include <cmath>
namespace Omega {
StrategyQ2_17::StrategyQ2_17() : var(0) {}
double StrategyQ2_17::compute(const Tick& t,const OrderBook& ob,const MicroState& ms,const double* base){
    double m=0.5*(t.bid+t.ask);
    var=0.95*var+0.05*(m*m);
    double vol=std::sqrt(std::max(0.0,var-m*m));
    double dsk=0; double B=ob.bidSize[2]; double A=ob.askSize[2];
    if(B+A>0) dsk=(B-A)/(B+A);
    double fuse=(base[5]+base[6])*0.5;
    return vol*0.45 + dsk*0.25 + ms.v[1]*0.20 + fuse*0.10;
}
}
