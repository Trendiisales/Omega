#include "Hybrid01.hpp"
#include <cmath>
namespace Omega {
Hybrid01::Hybrid01() : emaFast(0), emaSlow(0), var(0) {}
double Hybrid01::compute(const Tick& t,const OrderBook& ob,const MicroState& ms,
                         const double* base,const double* q2)
{
    double mid=0.5*(t.bid+t.ask);
    emaFast = 0.85*emaFast + 0.15*mid;
    emaSlow = 0.95*emaSlow + 0.05*mid;
    double trend = emaFast - emaSlow;

    // variance estimator
    var = 0.97*var + 0.03*((mid-emaSlow)*(mid-emaSlow));
    double volatility = std::sqrt(std::max(0.0,var));

    // orderbook pressure
    double press=0;
    double B = ob.bidSize[0] + ob.bidSize[1] + ob.bidSize[2];
    double A = ob.askSize[0] + ob.askSize[1] + ob.askSize[2];
    if(B+A>0) press = (B-A)/(B+A);

    // nonlinear microstructure fusion
    double m1 = std::tanh(ms.v[0] * 0.7 + ms.v[3] * 0.3);
    double m2 = std::tanh(ms.v[7] * 0.5 - ms.v[12] * 0.5);

    // fuse 8 strong base signals
    double baseFuse =
        0.12*(base[0] + base[2] + base[4] + base[6] +
              base[8] + base[10] + base[12] + base[14]);

    // fuse 8 high-impact Q2 signals
    double q2Fuse =
        0.12*(q2[1] + q2[3] + q2[5] + q2[7] +
              q2[9] + q2[11] + q2[13] + q2[15]);

    return trend*0.35
         + press*0.25
         + volatility*0.10
         + m1*0.10
         + m2*0.10
         + baseFuse*0.05
         + q2Fuse*0.05;
}
}
