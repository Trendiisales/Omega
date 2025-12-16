#pragma once
#include <string>
#include <functional>
#include "../net/WebSocketClient.hpp"
#include "../json/Json.hpp"

namespace Omega {

class BinanceWebSocketController {
public:
    BinanceWebSocketController();
    ~BinanceWebSocketController();

    bool connectDepth(const std::string& sym);
    bool connectTicker(const std::string& sym);

    void close();

    void setCallback(std::function<void(const std::string&)> cb);

private:
    WebSocketClient ws;
    std::function<void(const std::string&)> callback;

    static std::string toLower(const std::string&);
};

}
