#pragma once
#include <cstdint>
#include <atomic>
#include <mutex>
#include <string>
#include "../execution/OrderIntent.hpp"
#include "../fix/execution/FIXExecHandler.hpp"
#include "../engine/EngineConfig.hpp"

namespace Omega {

struct SupervisorConfig {
    int maxPos = 1;
    uint64_t cooldownMs = 50;
    double minConf = 0.01;
    double maxDailyLoss = -500;
};

struct FIXRejectInfo {
    std::string reason;
    int code = 0;
};

class ExecutionSupervisor {
public:
    ExecutionSupervisor();
    ~ExecutionSupervisor();

    // Configuration methods
    void configure(const SupervisorConfig& cfg);
    void init(const SupervisorConfig& cfg) { configure(cfg); }
    void init(const ExecConfig& cfg);  // For MotherEngine compatibility
    
    // Individual setters for compatibility
    void setSymbol(const std::string& s) { symbol = s; }
    void setMode(const std::string& m) { mode = m; }
    void setCoolDownMs(uint64_t ms) { config.cooldownMs = ms; }
    void setMinConfidence(double c) { config.minConf = c; }
    void setMaxPosition(int p) { config.maxPos = p; }

    // Approval methods
    bool approve(const OrderIntent& intent);
    bool approve(double signal);  // For simple signal-based approval

    // Execution callbacks
    void onExecution(const OrderIntent& intent, double fillPrice, double fillQty);
    void onExecution(const ExecReport& report);
    
    // Rejection callbacks
    void onReject(const OrderIntent& intent, const FIXRejectInfo& info);
    void onReject(const FIXRejectInfo& info);

    // Routing
    void route(const OrderIntent& order);

    // Stats
    void updatePnL(double delta);
    int execCount() const { return execs; }
    int rejectCount() const { return rejects; }
    double pnl() const { return dailyPnL; }
    int position() const { return pos; }

private:
    SupervisorConfig config;
    ExecConfig execCfg;
    std::string symbol;
    std::string mode;

    std::atomic<int> pos{0};
    std::atomic<int> execs{0};
    std::atomic<int> rejects{0};
    std::atomic<double> dailyPnL{0};

    uint64_t lastExecTs = 0;
    mutable std::mutex mtx;
};

} // namespace Omega
