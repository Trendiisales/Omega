// =============================================================================
// BinanceUnifiedFeed.hpp - Binance WebSocket Market Data Feed v6.2
// =============================================================================
// v6.2 HARDENING:
//   - State callback for BLIND MODE support
//   - Notifies engine on WS connect/disconnect for VenueState transitions
// =============================================================================
#pragma once
#include <string>
#include <functional>
#include "../market/Tick.hpp"
#include "../market/OrderBook.hpp"
#include "../binance/BinanceWebSocketController.hpp"
#include "../binance/BinanceTickNormalizer.hpp"
#include "../binance/BinanceDepthNormalizer.hpp"
#include "../json/Json.hpp"

namespace Chimera {

class BinanceUnifiedFeed {
public:
    BinanceUnifiedFeed();

    bool start(const std::string& symbol);
    void stop();

    void setTickCallback (std::function<void(const Tick&)> cb);
    void setBookCallback (std::function<void(const OrderBook&)> cb);
    
    // v6.2: State callback for BLIND MODE
    // Called with true on connect, false on disconnect
    void setStateCallback(std::function<void(bool)> cb);

private:
    BinanceWebSocketController ws;
    std::function<void(const Tick&)> tickCB;
    std::function<void(const OrderBook&)> bookCB;
    std::function<void(bool)> stateCB;  // v6.2: State callback

    OrderBook ob;
    Tick tick;

    void onMsg(const std::string&);
};

}
