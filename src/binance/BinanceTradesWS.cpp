#include "BinanceTradesWS.hpp"
#include <sstream>

namespace Omega {

BinanceTradesWS::BinanceTradesWS()
    : ready(false), lastPrice(0), lastQty(0), isBuyerMaker(false)
{}

bool BinanceTradesWS::connect(const std::string& symbol)
{
    sym = symbol;
    std::string stream = "/ws/" + sym + "@trade";

    ws.setOnMessage([this](const std::string& msg){
        handleMsg(msg);
    });

    ready = ws.connect(stream);
    return ready;
}

void BinanceTradesWS::handleMsg(const std::string& msg)
{
    std::lock_guard<std::mutex> lock(mtx);

    auto findStr=[&](const std::string& k)->std::string{
        auto p = msg.find("\""+k+"\":");
        if(p==std::string::npos) return "";
        p += k.size()+3;
        size_t e = p;
        while(e<msg.size() && msg[e]!=',' && msg[e]!='}' && msg[e]!='\"') e++;
        return msg.substr(p, e-p);
    };

    lastPrice = std::stod(findStr("p"));
    lastQty   = std::stod(findStr("q"));
    isBuyerMaker = (msg.find("\"m\":true") != std::string::npos);
}

bool BinanceTradesWS::poll(Tick& t)
{
    if(!ready) return false;

    std::lock_guard<std::mutex> lock(mtx);

    t.symbol = sym;
    t.bid = lastPrice;
    t.ask = lastPrice;
    t.spread = 0;
    t.delta = lastPrice * 0.00001;
    t.buyVol = isBuyerMaker ? 0 : lastQty;
    t.sellVol = isBuyerMaker ? lastQty : 0;
    t.liquidityGap = 0;
    t.b1=t.b2=t.a1=t.a2=0;
    t.ts = 0;

    return true;
}

}
