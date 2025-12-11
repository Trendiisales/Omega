#pragma once
#include <vector>
#include "FIXMDDecoder.hpp"
#include "../../market/OrderBook.hpp"

namespace Omega {

class FIXDepthNormalizer {
public:
    static void normalize(const std::vector<FIXMDEntry>&, OrderBook&);
};

}
