#include "Hybrid06.hpp"
#include <cmath>
namespace Omega {
Hybrid06::Hybrid06() : acc(0), var(0), lastMid(0) {}
double Hybrid06::compute(const Tick& t,const OrderBook& ob,const MicroState& ms,
                         const double* base,const double* q2)
{
    double m=0.5*(t.bid+t.ask);
    double d=m-lastMid; lastMid=m;

    acc = 0.9*acc + 0.1*(d*d);
    var = 0.95*var + 0.05*(m*m);

    double volatility = std::sqrt(std::max(0.0,var - m*m));

    double obI=0;
    double B=0,A=0;
    for(int i=0;i<5;i++){ B+=ob.bidSize[i]; A+=ob.askSize[i]; }
    if(B+A>0) obI=(B-A)/(B+A);

    double micro = std::tanh(ms.v[4] - ms.v[11]);

    double baseMix =
        0.12*(base[3]+base[6]+base[9]+base[12]+base[15]+base[18]+base[21]+base[24]);

    double q2mix =
        0.12*(q2[2]+q2[6]+q2[10]+q2[14]+q2[18]+q2[22]+q2[26]+q2[30]);

    return acc*0.30
         + volatility*0.20
         + obI*0.20
         + micro*0.10
         + baseMix*0.10
         + q2mix*0.10;
}
}
