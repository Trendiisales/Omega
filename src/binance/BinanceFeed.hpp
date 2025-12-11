#pragma once
#include <string>
#include <mutex>
#include <atomic>

#include "BinanceMarketDataWS.hpp"
#include "BinanceBookTickerWS.hpp"
#include "BinanceTradesWS.hpp"
#include "BinanceKlinesWS.hpp"

#include "../market/Tick.hpp"
#include "../market/OrderBook.hpp"

namespace Omega {

class BinanceFeed {
public:
    BinanceFeed();

    bool connect(const std::string& symbol);

    // Poll unified tick + book
    bool poll(Tick& t, OrderBook& ob);

    // Optional: poll last kline
    bool pollKline(Kline& k);

private:
    std::string sym;

    BinanceMarketDataWS depth;
    BinanceBookTickerWS bkt;
    BinanceTradesWS     trades;
    BinanceKlinesWS     kln;

    std::atomic<bool> readyDepth;
    std::atomic<bool> readyBkt;
    std::atomic<bool> readyTrades;
    std::atomic<bool> readyKln;

    std::mutex mtx;

    // shadow book
    double bidPx[10];
    double askPx[10];
    double bidSz[10];
    double askSz[10];

    // shadow ticker
    double lastBid;
    double lastAsk;
    double lastTrade;
    double buyVol;
    double sellVol;

    long lastTS;

    void mergeDepth(OrderBook& ob);
    void mergeTicker(Tick& t);
    void mergeTrades(Tick& t);
};

}
