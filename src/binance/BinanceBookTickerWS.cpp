#include "BinanceBookTickerWS.hpp"
#include <sstream>

namespace Omega {

BinanceBookTickerWS::BinanceBookTickerWS()
    : ready(false), bid(0), ask(0), bidQty(0), askQty(0)
{}

bool BinanceBookTickerWS::connect(const std::string& symbol)
{
    sym = symbol;
    std::string stream = "/ws/" + sym + "@bookTicker";

    ws.setOnMessage([this](const std::string& msg){
        handleMsg(msg);
    });

    ready = ws.connect(stream);
    return ready;
}

void BinanceBookTickerWS::handleMsg(const std::string& msg)
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

    bid = std::stod(findStr("b"));
    ask = std::stod(findStr("a"));
    bidQty = std::stod(findStr("B"));
    askQty = std::stod(findStr("A"));
}

bool BinanceBookTickerWS::poll(Tick& t)
{
    if(!ready) return false;
    std::lock_guard<std::mutex> lock(mtx);

    t.symbol = sym;
    t.bid = bid;
    t.ask = ask;
    t.spread = (ask>0 ? ask - bid : 0);
    t.delta = (ask+bid) * 0.00001;
    t.buyVol = bidQty;
    t.sellVol = askQty;
    t.liquidityGap = 0;
    t.b1=t.b2=t.a1=t.a2=0;
    t.ts = 0;

    return true;
}

}
