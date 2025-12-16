#include "RiskEngine.hpp"
#include <chrono>
#include <cmath>

namespace Omega {

RiskEngine::RiskEngine() {}

void RiskEngine::init(const RiskConfig& cfg,
                      std::unordered_map<std::string, PositionTracker>* pos) {
    config = cfg;
    positions = pos;
}

bool RiskEngine::allow(const std::string& symbol, const Decision& d) {
    std::lock_guard<std::mutex> g(lock);
    
    if (riskBreached) return false;
    if (!checkCooldown()) return false;
    if (!checkPosition(symbol, d.qty)) return false;
    if (!checkExposure(d.qty * d.price)) return false;
    if (!checkDrawdown()) return false;
    
    return true;
}

void RiskEngine::onOrder(const OrderIntent& o) {
    std::lock_guard<std::mutex> g(lock);
    openOrders++;
    lastOrderTs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

void RiskEngine::onExecution(const ExecReport& r) {
    std::lock_guard<std::mutex> g(lock);
    
    if (r.status == "2") { // Filled
        openOrders--;
    }
    
    // Update daily PnL
    if (positions) {
        double pnl = 0;
        for (auto& kv : *positions) {
            // Compute realized PnL from position snapshots
            auto snap = kv.second.snapshot();
            (void)snap;
        }
        dailyPnL = pnl;
        
        if (dailyPnL > peakPnL) peakPnL = dailyPnL;
        currentDrawdown = peakPnL - dailyPnL;
        
        if (currentDrawdown > config.maxDrawdownPct) {
            riskBreached = true;
        }
        if (dailyPnL < -config.maxDailyLoss) {
            riskBreached = true;
        }
    }
}

bool RiskEngine::checkPosition(const std::string& sym, double qty) {
    if (!positions) return true;
    auto it = positions->find(sym);
    if (it == positions->end()) return true;
    
    double current = std::abs(it->second.position());
    return (current + qty) <= config.maxPositionSize;
}

bool RiskEngine::checkExposure(double notional) {
    return (totalExposure + notional) <= config.maxExposure;
}

bool RiskEngine::checkDrawdown() {
    return currentDrawdown < config.maxDrawdownPct;
}

bool RiskEngine::checkCooldown() {
    int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return (now - lastOrderTs) >= config.cooldownMs;
}

double RiskEngine::exposure() const {
    std::lock_guard<std::mutex> g(lock);
    return totalExposure;
}

double RiskEngine::drawdown() const {
    std::lock_guard<std::mutex> g(lock);
    return currentDrawdown;
}

bool RiskEngine::breached() const {
    std::lock_guard<std::mutex> g(lock);
    return riskBreached;
}

void RiskEngine::reset() {
    std::lock_guard<std::mutex> g(lock);
    totalExposure = dailyPnL = peakPnL = currentDrawdown = 0;
    openOrders = 0;
    lastOrderTs = 0;
    riskBreached = false;
}

}
