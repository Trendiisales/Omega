#pragma once
#include <string>
#include <atomic>
#include <thread>
#include <functional>
#include <vector>
#include <memory>
#include "../data/UnifiedTick.hpp"
#include "../data/TickAssembler.hpp"
#include "../micro/MicroEngineBase.hpp"
#include "../micro/MicroEngineTrend.hpp"
#include "../micro/MicroEngineReversion.hpp"
#include "../micro/MicroEngineMomentum.hpp"
#include "../micro/MicroEngineBreakout.hpp"
#include "../micro/MicroEngineVolumeShock.hpp"
#include "../strategy/StrategyFusion.hpp"
#include "../supervisor/ExecutionSupervisor.hpp"
#include "../router/ExecutionRouter.hpp"

namespace Omega {

class OmegaEngine {
public:
    OmegaEngine();
    ~OmegaEngine();

    void setSymbol(const std::string& s);
    void setLogPath(const std::string& p);
    void setMode(const std::string& m);

    void init();
    void start();
    void stop();
    
    bool isRunning() const { return running; }
    
    // Stats
    uint64_t tickCount() const { return tickCount_; }
    uint64_t signalCount() const { return signalCount_; }

private:
    void tickLoop();
    void processTick(const UnifiedTick& t);

private:
    std::string symbol;
    std::string logPath;
    std::string mode;

    std::atomic<bool> running;
    std::thread tTick;

    TickAssembler assembler;

    MicroEngineTrend engTrend;
    MicroEngineReversion engReversion;
    MicroEngineMomentum engMomentum;
    MicroEngineBreakout engBreakout;
    MicroEngineVolumeShock engVolShock;

    StrategyFusion fusion;

    ExecutionSupervisor supervisor;
    ExecutionRouter router;
    
    std::atomic<uint64_t> tickCount_;
    std::atomic<uint64_t> signalCount_;
};

} // namespace Omega
