#include "Hybrid02.hpp"
#include <cmath>
namespace Omega {
Hybrid02::Hybrid02() : lastMid(0), momentum(0), shock(0) {}
double Hybrid02::compute(const Tick& t,const OrderBook& ob,const MicroState& ms,
                         const double* base,const double* q2)
{
    double m=0.5*(t.bid+t.ask);
    double d = m-lastMid; lastMid = m;

    momentum = 0.9*momentum + 0.1*d;
    shock    = 0.85*shock + 0.15*(t.buyVol + t.sellVol);

    double obImb=0;
    double B=0, A=0;
    for(int i=0;i<5;i++){ B+=ob.bidSize[i]; A+=ob.askSize[i]; }
    if(B+A>0) obImb = (B-A)/(B+A);

    double curve = std::tanh(ms.v[2]*0.6 - ms.v[9]*0.4);

    double baseBlk =
        0.10*(base[1]+base[5]+base[9]+base[13]+base[17]+base[21]+base[25]+base[29]);

    double q2Blk =
        0.10*(q2[2]+q2[6]+q2[10]+q2[14]+q2[18]+q2[22]+q2[26]+q2[30]);

    return momentum*0.30
         + shock*0.20
         + obImb*0.20
         + curve*0.10
         + baseBlk*0.10
         + q2Blk*0.10;
}
}
