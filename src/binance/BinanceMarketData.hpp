#pragma once
#include <string>
#include <functional>
#include <cstdint>
#include "../net/WebSocketClient.hpp"

namespace Omega {

struct BinanceTick {
    std::string symbol;
    double bid = 0.0;
    double ask = 0.0;
    double last = 0.0;
    double bidSize = 0.0;
    double askSize = 0.0;
    double lastSize = 0.0;
    uint64_t ts = 0;
};

class BinanceMarketData {
public:
    BinanceMarketData();
    ~BinanceMarketData();

    void attachWS(WebSocketClient* ws);

    void setCallback(std::function<void(const BinanceTick&)> cb);

    void subscribe(const std::string& symbol);

private:
    void onWs(const std::string& msg);
    BinanceTick parse(const std::string& json);

private:
    WebSocketClient* ws;
    std::function<void(const BinanceTick&)> onTick;
};

} // namespace Omega
