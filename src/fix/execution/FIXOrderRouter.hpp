#pragma once
#include <string>
#include <unordered_map>
#include <cstdint>
#include <functional>
#include "../transport/FIXTransport.hpp"
#include "../codec/FIXParser.hpp"

namespace Omega {

struct RoutedOrder {
    std::string symbol;
    std::string side;
    double qty = 0.0;
    double price = 0.0;
    std::string clOrdId;
    uint64_t ts = 0;
};

class FIXOrderRouter {
public:
    FIXOrderRouter();
    ~FIXOrderRouter();

    void attach(FIXTransport* t);

    void setAckCallback(std::function<void(const RoutedOrder&)> cb);
    void setFillCallback(std::function<void(const RoutedOrder&, double fillQty, double fillPx)> cb);
    void setCancelAckCallback(std::function<void(const std::string& clId)> cb);

    std::string sendLimit(const std::string& symbol,
                          const std::string& side,
                          double qty,
                          double price);

    bool sendCancel(const std::string& clOrdId);

private:
    void onRx(const std::string& msg);
    void handleExec(const std::unordered_map<std::string,std::string>& tags);

private:
    FIXTransport* tr;
    std::function<void(const RoutedOrder&)> onAck;
    std::function<void(const RoutedOrder&, double, double)> onFill;
    std::function<void(const std::string&)> onCancelAck;

    uint64_t counter;
};

} // namespace Omega
