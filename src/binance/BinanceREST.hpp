#pragma once
#include <string>
#include <functional>
#include <cstdint>
#include "../net/HttpClient.hpp"
#include "BinanceHMAC.hpp"

namespace Omega {

class BinanceREST {
public:
    BinanceREST();
    ~BinanceREST();

    void setKeys(const std::string& apiKey,
                 const std::string& secretKey);

    void setBaseUrl(const std::string& url);

    void setCallback(std::function<void(const std::string&)> cb);

    // Public endpoints (no auth)
    void getDepth(const std::string& symbol, int limit);
    void getTicker(const std::string& symbol);
    void getExchangeInfo();
    
    // Private endpoints (auth required)
    void getAccount();
    void getOpenOrders(const std::string& symbol);
    void getAllOpenOrders();

    void newOrder(const std::string& symbol,
                  const std::string& side,
                  const std::string& type,
                  double qty,
                  double price);
    
    void newMarketOrder(const std::string& symbol,
                        const std::string& side,
                        double qty);

    void cancelOrder(const std::string& symbol, 
                     const std::string& orderId);

private:
    std::string timestamp() const;
    std::string makeQuery(const std::string& qs);

private:
    std::string apiKey;
    BinanceHMAC hmac;
    std::string baseUrl;

    HttpClient http;
    std::function<void(const std::string&)> onReply;
};

} // namespace Omega
