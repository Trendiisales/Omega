#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include "../../market/Tick.hpp"
#include "../../market/OrderBook.hpp"
#include "../../market/Decision.hpp"
#include "../../strategy/StrategyFusion.hpp"
#include "../../pipeline/TickPipelineExt.hpp"
#include "../../pipeline/MicroMetrics.hpp"
#include "../../positions/PositionTracker.hpp"
#include "../../risk/RiskEngine.hpp"
#include "../../supervisor/ExecutionSupervisor.hpp"
#include "../../execution/OrderIntent.hpp"

namespace Omega {

class SymbolThread {
public:
    SymbolThread();
    ~SymbolThread();

    void init(const std::string& symbol,
              StrategyFusion* fusion,
              TickPipelineExt* pipeline,
              PositionTracker* pos,
              RiskEngine* risk,
              ExecutionSupervisor* exec);

    void start();
    void stop();

    void pushTick(const Tick& t);
    void pushBook(const OrderBook& ob);

private:
    std::string symbol;
    StrategyFusion* fusion = nullptr;
    TickPipelineExt* pipeline = nullptr;
    PositionTracker* pos = nullptr;
    RiskEngine* risk = nullptr;
    ExecutionSupervisor* exec = nullptr;

    std::thread th;
    std::atomic<bool> running;

    std::mutex qLock;
    Tick       lastTick;
    OrderBook  lastBook;
    bool       hasTick = false;
    bool       hasBook = false;

    void loop();
};

} // namespace Omega
