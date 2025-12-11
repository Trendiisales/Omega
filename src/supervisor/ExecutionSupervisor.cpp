#include "ExecutionSupervisor.hpp"
#include <chrono>
#include <cmath>

namespace Omega {

static uint64_t sup_now() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

ExecutionSupervisor::ExecutionSupervisor() {}

ExecutionSupervisor::~ExecutionSupervisor() {}

void ExecutionSupervisor::configure(const SupervisorConfig& cfg) {
    std::lock_guard<std::mutex> lock(mtx);
    config = cfg;
}

void ExecutionSupervisor::init(const ExecConfig& cfg) {
    std::lock_guard<std::mutex> lock(mtx);
    execCfg = cfg;
    // Map ExecConfig to internal config
    config.cooldownMs = cfg.minOrderIntervalMs;
}

bool ExecutionSupervisor::approve(const OrderIntent& intent) {
    std::lock_guard<std::mutex> lock(mtx);

    uint64_t now = sup_now();

    // Cooldown check
    if (now - lastExecTs < config.cooldownMs) {
        return false;
    }

    // Position limit check
    int sideVal = (intent.side == OrderSide::BUY) ? 1 : -1;
    int newPos = pos + sideVal;
    if (newPos > config.maxPos || newPos < -config.maxPos) {
        return false;
    }

    // Daily loss check
    if (dailyPnL < config.maxDailyLoss) {
        return false;
    }

    return true;
}

bool ExecutionSupervisor::approve(double signal) {
    std::lock_guard<std::mutex> lock(mtx);

    uint64_t now = sup_now();

    // Cooldown check
    if (now - lastExecTs < config.cooldownMs) {
        return false;
    }

    // Signal strength check
    if (std::fabs(signal) < config.minConf) {
        return false;
    }

    // Position limit check
    int sideVal = (signal > 0) ? 1 : -1;
    int newPos = pos + sideVal;
    if (newPos > config.maxPos || newPos < -config.maxPos) {
        return false;
    }

    // Daily loss check
    if (dailyPnL < config.maxDailyLoss) {
        return false;
    }

    return true;
}

void ExecutionSupervisor::onExecution(const OrderIntent& intent, double fillPrice, double fillQty) {
    std::lock_guard<std::mutex> lock(mtx);
    int sideVal = (intent.side == OrderSide::BUY) ? 1 : -1;
    pos = pos + sideVal;
    lastExecTs = sup_now();
    execs++;
    (void)fillPrice;
    (void)fillQty;
}

void ExecutionSupervisor::onExecution(const ExecReport& report) {
    std::lock_guard<std::mutex> lock(mtx);
    int sideVal = (report.side == "BUY" || report.side == "1") ? 1 : -1;
    pos = pos + sideVal;
    lastExecTs = sup_now();
    execs++;
}

void ExecutionSupervisor::onReject(const OrderIntent& intent, const FIXRejectInfo& info) {
    (void)intent;
    (void)info;
    rejects++;
}

void ExecutionSupervisor::onReject(const FIXRejectInfo& info) {
    (void)info;
    rejects++;
}

void ExecutionSupervisor::route(const OrderIntent& order) {
    // Placeholder - actual routing would go to exchange
    (void)order;
}

void ExecutionSupervisor::updatePnL(double delta) {
    dailyPnL = dailyPnL + delta;
}

} // namespace Omega
