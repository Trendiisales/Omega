#include "TickCSV.hpp"
#include <sstream>

namespace Omega {

std::string TickCSV::header() {
    return "ts,symbol,bid,ask,spread,buyVol,sellVol,liqGap,delta,b1,b2,a1,a2";
}

std::string TickCSV::encode(const Tick& t){
    std::ostringstream o;
    o<<t.ts<<","<<t.symbol<<","<<t.bid<<","<<t.ask<<","<<t.spread<<","
     <<t.buyVol<<","<<t.sellVol<<","<<t.liquidityGap<<","<<t.delta<<","
     <<t.b1<<","<<t.b2<<","<<t.a1<<","<<t.a2;
    return o.str();
}

}
