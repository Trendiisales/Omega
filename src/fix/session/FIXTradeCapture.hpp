#pragma once
#include <string>
#include <functional>
#include "../FIXMessage.hpp"

namespace Omega {

struct TradeReport {
    std::string tradeID;
    std::string orderID;
    std::string symbol;
    double price=0;
    double qty=0;
    long ts=0;
};

class FIXTradeCapture {
public:
    FIXTradeCapture();

    bool parse(const FIXMessage&, TradeReport&);
    void setCallback(std::function<void(const TradeReport&)> cb);

private:
    std::function<void(const TradeReport&)> callback;
};

}
