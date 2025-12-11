#pragma once
#include <string>
#include <atomic>
#include <mutex>
#include "../net/WebSocketClient.hpp"
#include "../market/Tick.hpp"
#include "../market/OrderBook.hpp"

namespace Omega {

class BinanceUnifiedWS {
public:
    BinanceUnifiedWS();

    bool connect(const std::string& symbol);
    bool poll(Tick& t, OrderBook& ob);

private:
    WebSocketClient ws;

    std::mutex mtx;
    std::atomic<bool> ready;

    std::string sym;
    std::string lastPayload;

    double bidPx[10], askPx[10];
    double bidSz[10], askSz[10];
    double lastBid, lastAsk;
    double buyVol, sellVol;
    long   lastTS;

    void reset();
    void handleMsg(const std::string& msg);
};

}
