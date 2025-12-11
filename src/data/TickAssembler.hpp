#pragma once
#include <string>
#include <cstdint>
#include <functional>
#include <mutex>
#include "UnifiedTick.hpp"
#include "../binance/BinanceMarketData.hpp"

namespace Omega {

class TickAssembler {
public:
    TickAssembler();
    ~TickAssembler();

    void pushFIX(double bid, double ask, double bidSz, double askSz, uint64_t ts);
    void pushBinance(const BinanceTick& t);

    UnifiedTick last() const;

    void setCallback(std::function<void(const UnifiedTick&)> cb);

private:
    mutable std::mutex mtx;
    UnifiedTick lastTick;
    std::function<void(const UnifiedTick&)> onTick;
};

} // namespace Omega
