#include "BinanceUnifiedWS.hpp"
#include <sstream>
#include <chrono>

namespace Omega {

BinanceUnifiedWS::BinanceUnifiedWS() : ready(false)
{
    reset();
}

void BinanceUnifiedWS::reset()
{
    lastBid=lastAsk=0;
    buyVol=sellVol=0;
    lastTS=0;
    for(int i=0;i<10;i++){
        bidPx[i]=askPx[i]=0;
        bidSz[i]=askSz[i]=0;
    }
}

bool BinanceUnifiedWS::connect(const std::string& symbol)
{
    sym = symbol;
    std::string stream =
        "/stream?streams=" +
        sym + "@depth10@100ms/" +
        sym + "@bookTicker/" +
        sym + "@trade";

    ws.setOnMessage([this](const std::string& msg){
        handleMsg(msg);
    });

    ready = ws.connect(stream);
    return ready;
}

void BinanceUnifiedWS::handleMsg(const std::string& msg)
{
    std::lock_guard<std::mutex> lock(mtx);
    lastPayload = msg;

    auto stamp=[&](){
        lastTS = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    };

    if(msg.find("depthUpdate") != std::string::npos)
    {
        stamp();

        auto parseLevels=[&](const std::string& key, double* px, double* sz){
            size_t p = msg.find("\""+key+"\":");
            if(p==std::string::npos) return;
            p = msg.find('[',p);
            if(p==std::string::npos) return;

            for(int i=0;i<10;i++){
                size_t q1 = msg.find('[',p);
                size_t q2 = msg.find(']',q1);
                if(q1==std::string::npos||q2==std::string::npos) break;

                std::string lvl = msg.substr(q1+1,q2-q1-1);
                std::stringstream ss(lvl);

                std::string A,B;
                std::getline(ss,A,',');
                std::getline(ss,B,',');

                px[i]=std::stod(A);
                sz[i]=std::stod(B);

                p=q2+1;
            }
        };
        parseLevels("bids", bidPx, bidSz);
        parseLevels("asks", askPx, askSz);

        lastBid = bidPx[0];
        lastAsk = askPx[0];
    }

    if(msg.find("\"bookTicker\"") != std::string::npos)
    {
        stamp();

        auto find=[&](const std::string& k)->double{
            auto p=msg.find("\""+k+"\":");
            if(p==std::string::npos) return 0;
            p+=k.size()+3;
            size_t e=p;
            while(e<msg.size() && msg[e]!=',' && msg[e]!=']' && msg[e]!='\"') e++;
            return std::stod(msg.substr(p,e-p));
        };
        lastBid = find("b");
        lastAsk = find("a");
        bidSz[0]=find("B");
        askSz[0]=find("A");
    }

    if(msg.find("@trade") != std::string::npos)
    {
        stamp();

        auto find=[&](const std::string& k)->double{
            auto p=msg.find("\""+k+"\":");
            if(p==std::string::npos) return 0;
            p+=k.size()+3;
            size_t e=p;
            while(e<msg.size() && msg[e]!=',' && msg[e]!='\"' && msg[e]!=']') e++;
            return std::stod(msg.substr(p,e-p));
        };

        double p = find("p");
        double q = find("q");
        bool maker = (msg.find("\"m\":true") != std::string::npos);

        lastBid = lastAsk = p;
        buyVol  = maker ? 0 : q;
        sellVol = maker ? q : 0;
    }
}

bool BinanceUnifiedWS::poll(Tick& t, OrderBook& ob)
{
    if(!ready) return false;

    std::lock_guard<std::mutex> lock(mtx);

    t.symbol = sym;
    t.bid = lastBid;
    t.ask = lastAsk;
    t.spread = (lastAsk>0 ? lastAsk-lastBid : 0);
    t.buyVol = buyVol;
    t.sellVol = sellVol;
    t.delta = (lastAsk+lastBid)*0.00001;
    t.liquidityGap = 0;
    t.b1=t.b2=t.a1=t.a2=0;
    t.ts = lastTS;

    for(int i=0;i<10;i++){
        ob.bidPrice[i]=bidPx[i];
        ob.askPrice[i]=askPx[i];
        ob.bidSize[i] =bidSz[i];
        ob.askSize[i] =askSz[i];
    }

    return true;
}

}
