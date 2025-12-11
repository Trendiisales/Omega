#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <atomic>
#include <mutex>
#include <thread>
#include <functional>
#include "../market/Tick.hpp"
#include "../market/OrderBook.hpp"
#include "../risk/RiskEngine.hpp"
#include "../positions/PositionTracker.hpp"
#include "../strategy/StrategyFusion.hpp"
#include "../pipeline/TickPipelineExt.hpp"
#include "../supervisor/ExecutionSupervisor.hpp"
#include "../execution/OrderIntent.hpp"
#include "EngineState.hpp"
#include "EngineConfig.hpp"

namespace Omega {

class MotherEngine {
public:
    MotherEngine();

    bool init(const EngineConfig& cfgIn);
    void start();
    void stop();

    void onExternalTick(const std::string& symbol, const Tick& t);
    void onExternalBook(const std::string& symbol, const OrderBook& ob);
    void onExternalExec(const ExecReport& r);
    void onExternalReject(const FIXRejectInfo& r);

    EngineState state;

private:
    EngineConfig cfg;

    std::atomic<bool> running;
    std::thread mainThread;

    std::unordered_map<std::string, Tick>      lastTick;
    std::unordered_map<std::string, OrderBook> lastBook;

    std::mutex tickLock;
    std::mutex bookLock;

    std::unordered_map<std::string, PositionTracker>    posTracker;
    std::unordered_map<std::string, StrategyFusion*>    strat;
    std::unordered_map<std::string, TickPipelineExt*>   pipe;

    ExecutionSupervisor execSup;
    RiskEngine          risk;

    void mainLoop();
    void processTick(const std::string& symbol, const Tick& t);
    void processBook(const std::string& symbol, const OrderBook& ob);
    void processDecision(const std::string& symbol, const Decision& d);
    void routeOrder(const std::string& symbol, const OrderIntent& o);
    void finalize();

    void loadSymbols();
    void buildPipelines();
    void buildFusion();
    void buildRisk();
    void buildExec();
};

} // namespace Omega
