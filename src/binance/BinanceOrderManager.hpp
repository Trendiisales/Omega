#pragma once
#include <string>
#include <unordered_map>
#include <functional>
#include <cstdint>
#include <mutex>
#include "../net/WebSocketClient.hpp"
#include "BinanceHMAC.hpp"

namespace Omega {

struct BinanceOrder {
    std::string orderId;
    std::string clientId;
    std::string symbol;
    std::string side;
    double qty = 0.0;
    double filled = 0.0;
    double price = 0.0;
    std::string status;
    uint64_t ts = 0;
};

class BinanceOrderManager {
public:
    BinanceOrderManager();
    ~BinanceOrderManager();

    void setKeys(const std::string& apiKey, const std::string& secret);
    void attachWS(WebSocketClient* ws);

    void setOrderCallback(std::function<void(const BinanceOrder&)> cb);
    void setErrorCallback(std::function<void(const std::string&)> cb);

    std::string sendLimit(const std::string& symbol,
                          const std::string& side,
                          double qty,
                          double price);
    
    std::string sendMarket(const std::string& symbol,
                           const std::string& side,
                           double qty);

    void cancel(const std::string& symbol, const std::string& clientId);
    void cancelAll(const std::string& symbol);
    
    BinanceOrder getOrder(const std::string& clientId) const;
    bool hasOrder(const std::string& clientId) const;

private:
    void onWsMessage(const std::string& msg);
    BinanceOrder parseOrder(const std::string& json);
    std::string generateClientId();

private:
    WebSocketClient* ws;
    std::string apiKey;
    BinanceHMAC hmac;

    mutable std::mutex lock;
    std::unordered_map<std::string, BinanceOrder> orders;

    std::function<void(const BinanceOrder&)> onOrder;
    std::function<void(const std::string&)> onError;

    uint64_t counter;
};

} // namespace Omega
