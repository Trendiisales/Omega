#pragma once
#include <vector>
#include <mutex>
#include "../../market/OrderBook.hpp"
#include "FIXMDDecoder.hpp"

namespace Omega {

class FIXPriceBook {
public:
    FIXPriceBook();

    void applySnapshot(const std::vector<FIXMDEntry>&);
    void applyIncremental(const std::vector<FIXMDEntry>&);

    OrderBook get() const;

private:
    mutable std::mutex lock;
    OrderBook ob;

    void applyEntry(const FIXMDEntry& e);
};

}
