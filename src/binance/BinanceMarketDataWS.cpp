#include "BinanceMarketDataWS.hpp"
#include <cstring>
#include <sstream>
#include <algorithm>

namespace Omega {

BinanceMarketDataWS::BinanceMarketDataWS() : ready(false)
{
    resetBook();
}

void BinanceMarketDataWS::resetBook()
{
    for(int i=0;i<10;i++){
        bidPx[i]=askPx[i]=0;
        bidSz[i]=askSz[i]=0;
    }
}

bool BinanceMarketDataWS::connect(const std::string& symbol)
{
    sym = symbol;

    std::string stream = "/ws/" + sym + "@depth10@100ms";

    ws.setOnMessage([this](const std::string& msg){
        handleMsg(msg);
    });

    ready = ws.connect(stream);

    return ready;
}

void BinanceMarketDataWS::handleMsg(const std::string& msg)
{
    std::lock_guard<std::mutex> lock(mtx);
    lastPayload = msg;

    auto findVal=[&](const std::string& k)->std::string{
        auto pos = msg.find("\""+k+"\":");
        if(pos==std::string::npos) return "";
        pos += k.size()+3;
        while(pos<msg.size() && (msg[pos]==' ' || msg[pos]=='\"')) pos++;
        size_t end = pos;
        while(end<msg.size() && msg[end] != ',' && msg[end] != ']' && msg[end] != '\"') end++;
        return msg.substr(pos, end-pos);
    };

    double bp[10]{}, ap[10]{}, bs[10]{}, as[10]{};

    auto parseLevels=[&](const std::string& key,double* px,double* sz){
        size_t p = msg.find("\""+key+"\":");
        if(p==std::string::npos) return;
        p = msg.find('[',p);
        if(p==std::string::npos) return;
        for(int i=0;i<10;i++){
            size_t q1 = msg.find('[',p);
            size_t q2 = msg.find(']',q1);
            if(q1==std::string::npos || q2==std::string::npos) break;

            std::string lvl = msg.substr(q1+1, q2-q1-1);
            std::stringstream ss(lvl);
            std::string A,B;
            std::getline(ss,A,',');
            std::getline(ss,B,',');

            px[i] = std::stod(A);
            sz[i] = std::stod(B);

            p = q2+1;
        }
    };

    parseLevels("bids", bp, bs);
    parseLevels("asks", ap, as);

    for(int i=0;i<10;i++){
        bidPx[i]=bp[i];
        bidSz[i]=bs[i];
        askPx[i]=ap[i];
        askSz[i]=as[i];
    }
}

bool BinanceMarketDataWS::poll(Tick& t, OrderBook& ob)
{
    if(!ready) return false;

    std::lock_guard<std::mutex> lock(mtx);

    t.symbol = sym;
    t.bid = bidPx[0];
    t.ask = askPx[0];
    t.spread = (t.ask>0? t.ask-t.bid : 0);
    t.delta = (t.bid + t.ask) * 0.00001;
    t.buyVol = 0;
    t.sellVol = 0;
    t.liquidityGap = 0;
    t.b1=t.b2=t.a1=t.a2=0;
    t.ts = 0;

    for(int i=0;i<10;i++){
        ob.bidPrice[i]=bidPx[i];
        ob.askPrice[i]=askPx[i];
        ob.bidSize[i] =bidSz[i];
        ob.askSize[i] =askSz[i];
    }

    return true;
}

}
