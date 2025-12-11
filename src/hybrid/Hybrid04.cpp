#include "Hybrid04.hpp"
#include <cmath>
namespace Omega {
Hybrid04::Hybrid04() : ema(0), var(0) {}
double Hybrid04::compute(const Tick& t,const OrderBook& ob,const MicroState& ms,
                         const double* base,const double* q2)
{
    double mid=0.5*(t.bid+t.ask);
    ema = 0.9*ema + 0.1*mid;

    double d = mid-ema;
    var = 0.95*var + 0.05*(d*d);
    double vol = std::sqrt(std::max(0.0,var));

    double obImp = 0;
    double B=0,A=0;
    for(int i=0;i<5;i++){ B+=ob.bidSize[i]; A+=ob.askSize[i]; }
    if(B+A>0) obImp=(B-A)/(B+A);

    double microEntropy = std::tanh(ms.v[1] - ms.v[10]);
    double baseMix =
        0.12*(base[2]+base[4]+base[6]+base[8]+base[10]+base[12]+base[14]+base[16]);

    double q2mix =
        0.12*(q2[3]+q2[5]+q2[7]+q2[9]+q2[11]+q2[13]+q2[15]+q2[17]);

    return d*0.30
         + vol*0.20
         + obImp*0.20
         + microEntropy*0.10
         + baseMix*0.10
         + q2mix*0.10;
}
}
