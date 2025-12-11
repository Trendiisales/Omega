#pragma once
#include <string>
#include <vector>
#include <mutex>
#include "../micro/MicroEngineBase.hpp"
#include "../market/Tick.hpp"
#include "../pipeline/MicroMetrics.hpp"
#include "../positions/PositionTracker.hpp"
#include "../engine/EngineConfig.hpp"
#include "Decision.hpp"
#include "StrategyState.hpp"

namespace Omega {

class StrategyFusion {
public:
    StrategyFusion();
    ~StrategyFusion();

    // New interface (for OmegaEngine)
    void setSymbol(const std::string& s);
    void add(const MicroSignal& s);
    double compute();
    void reset();

    // Legacy interface (for existing code)
    void init(const std::string& symbol,
              const std::vector<std::string>& strategies);
    
    void init(const std::string& symbol,
              const StrategySet& cfg);

    Decision compute(const Tick& t,
                     const MicroMetrics& m,
                     PositionTracker& pos);

private:
    double computeBase(const MicroMetrics& m);
    double computeQ2(const MicroMetrics& m);
    double computeHybrid(const MicroMetrics& m);

private:
    std::string sym;
    std::vector<std::string> names;
    StrategySet config;
    
    // For new interface
    mutable std::mutex lock;
    std::vector<MicroSignal> buffer;
    StrategyState state;
    
    double threshold;
};

} // namespace Omega
