#pragma once
#include <string>
#include "../market/Tick.hpp"
#include "../market/OrderBook.hpp"
#include "../micro/MicroStructureState.hpp"

namespace Omega {

struct UnifiedRecord {
    long ts=0;

    Tick t;
    OrderBook ob;
    MicroStructureState m;

    void syncTS();
    static std::string header();
    static std::string encode(const UnifiedRecord&);
};

}
