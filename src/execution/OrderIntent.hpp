#pragma once
#include <string>

namespace Omega {

enum class OrderSide { BUY, SELL, NONE };
enum class OrderType { MARKET, LIMIT, IOC, FOK };

struct OrderIntent {
    std::string symbol;
    OrderSide side = OrderSide::NONE;
    OrderType type = OrderType::MARKET;
    double qty = 0.0;
    double price = 0.0;
    int64_t ts = 0;
    std::string clientId;
    
    bool valid() const {
        return side != OrderSide::NONE && qty > 0;
    }
};

}
