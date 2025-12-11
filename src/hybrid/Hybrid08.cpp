#include "Hybrid08.hpp"
#include <cmath>
namespace Omega {
Hybrid08::Hybrid08() : last(0), mom(0), shock(0) {}
double Hybrid08::compute(const Tick& t,const OrderBook& ob,const MicroState& ms,
                         const double* base,const double* q2)
{
    double px=0.5*(t.bid+t.ask);
    double d=px-last; last=px;

    mom   = 0.9*mom + 0.1*d;
    shock = 0.85*shock + 0.15*(t.buyVol + t.sellVol);

    double obP=0;
    double B=0,A=0; for(int i=0;i<5;i++){ B+=ob.bidSize[i]; A+=ob.askSize[i]; }
    if(B+A>0) obP=(B-A)/(B+A);

    double micro = std::tanh(ms.v[8] - ms.v[4]);

    double baseMixin =
        0.10*(base[7]+base[14]+base[21]+base[28]+base[3]+base[10]+base[17]+base[24]);

    double q2Mixin =
        0.10*(q2[2]+q2[7]+q2[12]+q2[17]+q2[22]+q2[27]+q2[29]+q2[31]);

    return mom*0.25
         + shock*0.20
         + obP*0.20
         + micro*0.10
         + baseMixin*0.10
         + q2Mixin*0.10;
}
}
