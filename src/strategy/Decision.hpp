#pragma once
#include <cstdint>
#include <string>

namespace Omega {

enum class Side {
    None,
    Buy,
    Sell
};

// Aliases for old code
constexpr Side BUY = Side::Buy;
constexpr Side SELL = Side::Sell;

struct Decision {
    bool   valid  = false;
    Side   side   = Side::None;
    double qty    = 0.0;
    double price  = 0.0;
    double score  = 0.0;
    double conf   = 0.0;
    std::uint64_t ts = 0;
    std::uint64_t metaId = 0;
    
    std::string sideStr() const {
        if (side == Side::Buy) return "BUY";
        if (side == Side::Sell) return "SELL";
        return "";
    }
};

} // namespace Omega
