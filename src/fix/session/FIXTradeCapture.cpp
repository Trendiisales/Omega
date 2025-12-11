#include "FIXTradeCapture.hpp"
#include <cstdlib>

namespace Omega {

FIXTradeCapture::FIXTradeCapture(){}

void FIXTradeCapture::setCallback(std::function<void(const TradeReport&)> cb){
    callback=cb;
}

bool FIXTradeCapture::parse(const FIXMessage& msg, TradeReport& r){
    if(msg.get(35)!="AE") return false; // TradeCaptureReport

    r.tradeID = msg.get(17);
    r.orderID = msg.get(37);
    r.symbol  = msg.get(55);
    r.price   = atof(msg.get(31).c_str());
    r.qty     = atof(msg.get(32).c_str());
    r.ts      = atol(msg.get(60).c_str());

    if(callback) callback(r);
    return true;
}

}
