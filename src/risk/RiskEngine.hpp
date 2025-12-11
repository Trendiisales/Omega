#pragma once
#include <string>
#include <unordered_map>
#include <mutex>
#include "../strategy/Decision.hpp"
#include "../execution/OrderIntent.hpp"
#include "../positions/PositionTracker.hpp"
#include "../engine/EngineConfig.hpp"
#include "../fix/execution/FIXExecHandler.hpp"

namespace Omega {

class RiskEngine {
public:
    RiskEngine();
    
    void init(const RiskConfig& cfg, 
              std::unordered_map<std::string, PositionTracker>* pos);
    
    bool allow(const std::string& symbol, const Decision& d);
    void onOrder(const OrderIntent& o);
    void onExecution(const ExecReport& r);
    
    double exposure() const;
    double drawdown() const;
    bool breached() const;
    
    void reset();

private:
    RiskConfig config;
    std::unordered_map<std::string, PositionTracker>* positions = nullptr;
    
    mutable std::mutex lock;
    double totalExposure = 0.0;
    double dailyPnL = 0.0;
    double peakPnL = 0.0;
    double currentDrawdown = 0.0;
    int openOrders = 0;
    int64_t lastOrderTs = 0;
    bool riskBreached = false;
    
    bool checkPosition(const std::string& sym, double qty);
    bool checkExposure(double notional);
    bool checkDrawdown();
    bool checkCooldown();
};

}
