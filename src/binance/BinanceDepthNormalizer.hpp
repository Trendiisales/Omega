#pragma once
#include <vector>
#include <utility>
#include "../market/OrderBook.hpp"

namespace Omega {

class BinanceDepthNormalizer {
public:
    static void toOrderBook(const std::vector<std::pair<double,double>>& bids,
                            const std::vector<std::pair<double,double>>& asks,
                            OrderBook& ob);
};

}
