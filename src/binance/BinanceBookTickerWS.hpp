#pragma once
#include <string>
#include <mutex>
#include <atomic>

#include "../net/WebSocketClient.hpp"
#include "../market/Tick.hpp"

namespace Omega {

class BinanceBookTickerWS {
public:
    BinanceBookTickerWS();

    bool connect(const std::string& symbol);
    bool poll(Tick& t);

private:
    WebSocketClient ws;
    std::string sym;

    std::mutex mtx;
    std::atomic<bool> ready;

    double bid;
    double ask;
    double bidQty;
    double askQty;

    void handleMsg(const std::string& msg);
};

}
