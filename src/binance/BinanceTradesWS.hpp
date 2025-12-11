#pragma once
#include <string>
#include <mutex>
#include <atomic>

#include "../net/WebSocketClient.hpp"
#include "../market/Tick.hpp"

namespace Omega {

class BinanceTradesWS {
public:
    BinanceTradesWS();

    bool connect(const std::string& symbol);
    bool poll(Tick& t);

private:
    WebSocketClient ws;
    std::string sym;

    std::mutex mtx;
    std::atomic<bool> ready;

    double lastPrice;
    double lastQty;
    bool isBuyerMaker;

    void handleMsg(const std::string& msg);
};

}
