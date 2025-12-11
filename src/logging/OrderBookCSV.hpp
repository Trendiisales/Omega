#pragma once
#include <string>
#include "../market/OrderBook.hpp"

namespace Omega {

class OrderBookCSV {
public:
    static std::string header();
    static std::string encode(const OrderBook&);
};

}
