#include "OrderBookCSV.hpp"
#include <sstream>

namespace Omega {

std::string OrderBookCSV::header(){
    return "ts,"
           "b1p,b1s,b2p,b2s,b3p,b3s,b4p,b4s,b5p,b5s,"
           "b6p,b6s,b7p,b7s,b8p,b8s,b9p,b9s,b10p,b10s,"
           "a1p,a1s,a2p,a2s,a3p,a3s,a4p,a4s,a5p,a5s,"
           "a6p,a6s,a7p,a7s,a8p,a8s,a9p,a9s,a10p,a10s";
}

std::string OrderBookCSV::encode(const OrderBook& ob){
    std::ostringstream o;
    o<<ob.ts;
    for(int i=0;i<10;i++){
        o<<","<<ob.bidPrice[i]<<","<<ob.bidSize[i];
    }
    for(int i=0;i<10;i++){
        o<<","<<ob.askPrice[i]<<","<<ob.askSize[i];
    }
    return o.str();
}

}
