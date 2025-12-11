#pragma once
#include <string>
#include <vector>
#include "../market/Tick.hpp"
#include "../pipeline/MicroMetrics.hpp"
#include "../positions/PositionTracker.hpp"
#include "../engine/EngineConfig.hpp"
#include "Decision.hpp"

namespace Omega {

class StrategyFusion {
public:
    StrategyFusion();

    void init(const std::string& symbol,
              const std::vector<std::string>& strategies);
    
    void init(const std::string& symbol,
              const StrategySet& cfg);

    Decision compute(const Tick& t,
                     const MicroMetrics& m,
                     PositionTracker& pos);

private:
    std::string sym;
    std::vector<std::string> names;
    StrategySet config;
    
    double baseScores[32] = {0};
    double q2Scores[32] = {0};
    double hybridScores[8] = {0};
    
    double computeBase(const Tick& t, const MicroMetrics& m);
    double computeQ2(const Tick& t, const MicroMetrics& m);
    double computeHybrid(const Tick& t, const MicroMetrics& m);
};

} // namespace Omega
