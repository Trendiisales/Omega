#pragma once
#include <cstring>
#include <string>
#include <mutex>
#include <atomic>
#include "../data/UnifiedTick.hpp"
#include "../market/Tick.hpp"
#include "../market/OrderBook.hpp"
#include "../positions/PositionTracker.hpp"
#include "data/MLLogger.hpp"

namespace Omega {

struct SymbolThreadState {
    std::string symbol;
    
    // Market data
    UnifiedTick lastTick;
    Tick tick;
    OrderBook book;
    
    // Engine state
    MicroState micro;
    StrategyState32 strategy;
    EngineState engine;
    
    // Position/PnL
    double pnl = 0.0;
    int tradeCount = 0;
    PositionTracker position;
    
    std::atomic<bool> active{false};
    std::mutex mtx;
    
    SymbolThreadState() {
        memset(&micro, 0, sizeof(micro));
        memset(&strategy, 0, sizeof(strategy));
        memset(&engine, 0, sizeof(engine));
    }
    
    void reset() {
        std::lock_guard<std::mutex> lock(mtx);
        pnl = 0.0;
        tradeCount = 0;
        position.reset();
        memset(&micro, 0, sizeof(micro));
        memset(&strategy, 0, sizeof(strategy));
    }
};

} // namespace Omega
