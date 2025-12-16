#pragma once
#include <string>
#include <functional>
#include "../market/Tick.hpp"
#include "../market/OrderBook.hpp"
#include "../binance/BinanceWebSocketController.hpp"
#include "../binance/BinanceTickNormalizer.hpp"
#include "../binance/BinanceDepthNormalizer.hpp"
#include "../json/Json.hpp"

namespace Omega {

class BinanceUnifiedFeed {
public:
    BinanceUnifiedFeed();

    bool start(const std::string& symbol);
    void stop();

    void setTickCallback (std::function<void(const Tick&)> cb);
    void setBookCallback (std::function<void(const OrderBook&)> cb);

private:
    BinanceWebSocketController ws;
    std::function<void(const Tick&)> tickCB;
    std::function<void(const OrderBook&)> bookCB;

    OrderBook ob;
    Tick tick;

    void onMsg(const std::string&);
};

}
