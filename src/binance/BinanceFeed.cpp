#include "BinanceFeed.hpp"
#include <chrono>
#include <cstring>

namespace Omega {

BinanceFeed::BinanceFeed()
    : readyDepth(false), readyBkt(false),
      readyTrades(false), readyKln(false),
      lastBid(0), lastAsk(0), lastTrade(0),
      buyVol(0), sellVol(0), lastTS(0)
{
    for(int i=0;i<10;i++){
        bidPx[i]=askPx[i]=0;
        bidSz[i]=askSz[i]=0;
    }
}

bool BinanceFeed::connect(const std::string& symbol)
{
    sym = symbol;

    readyDepth  = depth.connect(sym);
    readyBkt    = bkt.connect(sym);
    readyTrades = trades.connect(sym);
    readyKln    = kln.connect(sym, "1m");

    return readyDepth || readyBkt || readyTrades || readyKln;
}

void BinanceFeed::mergeDepth(OrderBook& ob)
{
    for(int i=0;i<10;i++){
        ob.bidPrice[i]=bidPx[i];
        ob.askPrice[i]=askPx[i];
        ob.bidSize[i] =bidSz[i];
        ob.askSize[i] =askSz[i];
    }
}

void BinanceFeed::mergeTicker(Tick& t)
{
    t.bid = lastBid;
    t.ask = lastAsk;
    t.spread = (lastAsk > 0 ? lastAsk - lastBid : 0);
}

void BinanceFeed::mergeTrades(Tick& t)
{
    t.delta = lastTrade * 0.00001;
    t.buyVol  = buyVol;
    t.sellVol = sellVol;
}

bool BinanceFeed::poll(Tick& t, OrderBook& ob)
{
    std::lock_guard<std::mutex> lock(mtx);

    Tick dt;
    OrderBook odb;

    bool ok1 = depth.poll(dt, odb);

    if(ok1){
        for(int i=0;i<10;i++){
            bidPx[i]=odb.bidPrice[i];
            askPx[i]=odb.askPrice[i];
            bidSz[i]=odb.bidSize[i];
            askSz[i]=odb.askSize[i];
        }
        lastBid = dt.bid;
        lastAsk = dt.ask;
    }

    Tick bt;
    bool ok2 = bkt.poll(bt);
    if(ok2){
        lastBid = bt.bid;
        lastAsk = bt.ask;
    }

    Tick tr;
    bool ok3 = trades.poll(tr);
    if(ok3){
        lastTrade = tr.bid;
        buyVol    = tr.buyVol;
        sellVol   = tr.sellVol;
    }

    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    lastTS = now;

    t.symbol = sym;
    t.ts = lastTS;

    mergeTicker(t);
    mergeTrades(t);

    mergeDepth(ob);

    t.liquidityGap = 0;
    t.b1=t.b2=t.a1=t.a2=0;

    return ok1 || ok2 || ok3;
}

bool BinanceFeed::pollKline(Kline& k)
{
    return kln.poll(k);
}

}
