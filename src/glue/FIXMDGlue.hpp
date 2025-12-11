#pragma once
#include <string>
#include <functional>
#include "../fix/marketdata/FIXMDOrderBook.hpp"
#include "../fix/marketdata/FIXMDNormalizer.hpp"
#include "../data/UnifiedTick.hpp"

namespace Omega {

class FIXMDGlue {
public:
    FIXMDGlue();
    ~FIXMDGlue();

    void attach(FIXMDOrderBook* b);
    void setSymbol(const std::string& s);
    void setCallback(std::function<void(const UnifiedTick&)> cb);

    void onFIXUpdate(double bid,
                     double ask,
                     double bidSize,
                     double askSize,
                     uint64_t ts);

private:
    FIXMDOrderBook* book;
    FIXMDNormalizer normalizer;
    std::string symbol;

    std::function<void(const UnifiedTick&)> onTick;
};

} // namespace Omega
