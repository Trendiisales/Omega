#include "BinanceKlinesWS.hpp"
#include <sstream>

namespace Omega {

BinanceKlinesWS::BinanceKlinesWS() : ready(false)
{
    last = {0,0,0,0,0,0};
}

bool BinanceKlinesWS::connect(const std::string& symbol,
                              const std::string& interval)
{
    sym = symbol;
    intv = interval;

    std::string stream = "/ws/" + sym + "@kline_" + interval;

    ws.setOnMessage([this](const std::string& msg){
        handleMsg(msg);
    });

    ready = ws.connect(stream);
    return ready;
}

void BinanceKlinesWS::handleMsg(const std::string& msg)
{
    std::lock_guard<std::mutex> lock(mtx);

    auto findVal=[&](const std::string& tag)->std::string{
        auto p = msg.find("\""+tag+"\":");
        if(p==std::string::npos) return "";
        p += tag.size()+3;
        while(p<msg.size() && (msg[p]==' ' || msg[p]=='\"')) p++;
        size_t e=p;
        while(e<msg.size() && msg[e]!=',' && msg[e]!='\"' && msg[e]!='}') e++;
        return msg.substr(p,e-p);
    };

    last.open  = std::stod(findVal("o"));
    last.high  = std::stod(findVal("h"));
    last.low   = std::stod(findVal("l"));
    last.close = std::stod(findVal("c"));
    last.volume= std::stod(findVal("v"));
    last.ts    = std::stol(findVal("T"));
}

bool BinanceKlinesWS::poll(Kline& k)
{
    if(!ready) return false;

    std::lock_guard<std::mutex> lock(mtx);
    k = last;
    return true;
}

}
