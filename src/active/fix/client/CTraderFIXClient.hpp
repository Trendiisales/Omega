#pragma once

#include <string>
#include <functional>
#include <atomic>

namespace Chimera {

struct CTraderConfig {
    std::string host;
    int port = 0;

    std::string senderCompID;
    std::string targetCompID;
    std::string username;
    std::string password;
};

using FIXConfig = CTraderConfig;

class CTraderFIXClient {
public:
    using MDCallback = std::function<
        void(const std::string&, double, double, double, double)
    >;

    CTraderFIXClient();
    ~CTraderFIXClient();

    void setConfig(const FIXConfig& cfg);

    bool connect();
    void disconnect();

    bool isLoggedOn() const;

    void subscribeMarketData(const std::string& symbol);
    void unsubscribeMarketData(const std::string& symbol);

    void sendNewOrder(
        const std::string& clOrdID,
        const std::string& symbol,
        char side,
        double qty,
        double price = 0.0,
        char ordType = 'M'
    );

    void setMDCallback(MDCallback cb);

private:
    FIXConfig config_;
    MDCallback mdCallback_;
    std::atomic<bool> loggedOn_;
};

} // namespace Chimera
