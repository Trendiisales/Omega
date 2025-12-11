#include "Hybrid07.hpp"
#include <cmath>
namespace Omega {
Hybrid07::Hybrid07() : drift(0), mavg(0), last(0) {}
double Hybrid07::compute(const Tick& t,const OrderBook& ob,const MicroState& ms,
                         const double* base,const double* q2)
{
    double px=0.5*(t.bid+t.ask);
    double d=px-last; last=px;

    drift = 0.93*drift + 0.07*d;
    mavg  = 0.9*mavg + 0.1*px;

    double curvature = d - (px - mavg);

    double depth=0;
    double B=0,A=0; for(int i=0;i<5;i++){ B+=ob.bidSize[i]; A+=ob.askSize[i]; }
    if(B+A>0) depth=(B-A)/(B+A);

    double micro = std::tanh(ms.v[7] + ms.v[6] - ms.v[3]);

    double baseFuse =
        0.10*(base[4]+base[8]+base[12]+base[16]+base[20]+base[24]+base[28]+base[0]);

    double q2Fuse =
        0.10*(q2[1]+q2[5]+q2[9]+q2[13]+q2[17]+q2[21]+q2[25]+q2[29]);

    return drift*0.25
         + curvature*0.25
         + depth*0.20
         + micro*0.10
         + baseFuse*0.10
         + q2Fuse*0.10;
}
}
