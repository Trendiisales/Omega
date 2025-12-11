#include "Hybrid05.hpp"
#include <cmath>
namespace Omega {
Hybrid05::Hybrid05() : drift(0), last(0), impulse(0) {}
double Hybrid05::compute(const Tick& t,const OrderBook& ob,const MicroState& ms,
                         const double* base,const double* q2)
{
    double px=0.5*(t.bid+t.ask);
    double d = px-last; last=px;

    drift = 0.92*drift + 0.08*d;
    impulse = 0.88*impulse + 0.12*(t.buyVol - t.sellVol);

    double obP=0;
    double B=0,A=0; for(int i=0;i<5;i++){ B+=ob.bidSize[i]; A+=ob.askSize[i]; }
    if(B+A>0) obP=(B-A)/(B+A);

    double micro = std::tanh(ms.v[6] + ms.v[3] - ms.v[14]);

    double baseBlock =
        0.10*(base[1]+base[4]+base[7]+base[11]+base[13]+base[17]+base[21]+base[30]);

    double q2Block =
        0.10*(q2[0]+q2[4]+q2[8]+q2[12]+q2[16]+q2[20]+q2[24]+q2[28]);

    return drift*0.30
         + impulse*0.20
         + obP*0.20
         + micro*0.10
         + baseBlock*0.10
         + q2Block*0.10;
}
}
