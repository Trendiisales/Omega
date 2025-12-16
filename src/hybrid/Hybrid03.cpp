#include "Hybrid03.hpp"
#include <cmath>
namespace Omega {
Hybrid03::Hybrid03() : drift(0), acc(0), last(0) {}
double Hybrid03::compute(const Tick& t,const OrderBook& ob,const MicroState& ms,
                         const double* base,const double* q2)
{
    double px = 0.5*(t.bid+t.ask);
    double d  = px-last; last=px;

    drift = 0.92*drift + 0.08*d;
    acc   = 0.90*acc   + 0.10*(d*d);

    double depth=0;
    double B=0,A=0;
    for(int i=0;i<5;i++){ B+=ob.bidSize[i]; A+=ob.askSize[i]; }
    if(B+A>0) depth=(B-A)/(B+A);

    double entropy = std::tanh(ms.v[5] - ms.v[14]);

    double baseMix =
        0.11*(base[0]+base[3]+base[7]+base[11]+base[15]+base[19]+base[23]+base[27]);

    double q2Mix =
        0.11*(q2[1]+q2[4]+q2[8]+q2[12]+q2[16]+q2[20]+q2[24]+q2[28]);

    return drift*0.30
         + acc*0.20
         + depth*0.20
         + entropy*0.10
         + baseMix*0.10
         + q2Mix*0.10;
}
}
