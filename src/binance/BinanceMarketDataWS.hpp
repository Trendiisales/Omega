#pragma once
#include <string>
#include <mutex>
#include <atomic>

#include "../net/WebSocketClient.hpp"
#include "../market/Tick.hpp"
#include "../market/OrderBook.hpp"

namespace Omega {

class BinanceMarketDataWS {
public:
    BinanceMarketDataWS();

    bool connect(const std::string& symbol);
    bool poll(Tick& t, OrderBook& ob);

private:
    WebSocketClient ws;

    std::string sym;
    std::atomic<bool> ready;

    std::mutex mtx;

    // updated on every WS msg
    double bidPx[10];
    double askPx[10];
    double bidSz[10];
    double askSz[10];

    std::string lastPayload;

    void handleMsg(const std::string& msg);
    void resetBook();
};

}
