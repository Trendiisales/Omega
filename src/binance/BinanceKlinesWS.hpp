#pragma once
#include <string>
#include <mutex>
#include <atomic>

#include "../net/WebSocketClient.hpp"

namespace Omega {

struct Kline {
    double open;
    double high;
    double low;
    double close;
    double volume;
    long   ts;
};

class BinanceKlinesWS {
public:
    BinanceKlinesWS();

    bool connect(const std::string& symbol, const std::string& interval);
    bool poll(Kline& k);

private:
    WebSocketClient ws;

    std::string sym;
    std::string intv;

    std::mutex mtx;
    std::atomic<bool> ready;

    Kline last;

    void handleMsg(const std::string& msg);
};

}
