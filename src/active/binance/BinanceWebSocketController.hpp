// =============================================================================
// BinanceWebSocketController.hpp - Binance WebSocket Controller v6.2
// =============================================================================
// v6.2: Added setStateCallback for BLIND MODE support
// =============================================================================
#pragma once
#include <string>
#include <functional>
#include "../net/SSLWebSocketClient.hpp"
#include "../json/Json.hpp"

namespace Chimera {

class BinanceWebSocketController {
public:
    BinanceWebSocketController();
    ~BinanceWebSocketController();

    bool connectDepth(const std::string& sym);
    bool connectTicker(const std::string& sym);
    bool connectCombined(const std::string& sym);  // depth + trades

    void close();

    void setCallback(std::function<void(const std::string&)> cb);
    
    // v6.2: State callback for BLIND MODE
    void setStateCallback(std::function<void(bool)> cb);
    
    bool isConnected() const { return ws.isConnected(); }

private:
    SSLWebSocketClient ws;
    std::function<void(const std::string&)> callback;
    std::function<void(bool)> stateCallback;  // v6.2

    static std::string toLower(const std::string&);
};

}
