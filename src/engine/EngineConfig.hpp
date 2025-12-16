#pragma once
#include <string>
#include <vector>
#include <functional>
#include <cstdint>

namespace Omega {

struct RiskConfig {
    double maxNotionalPerSymbol = 1e7;
    double maxGlobalNotional    = 5e7;
    double maxDrawdownPct       = 0.2;
    double maxPositionSize      = 1.0;
    double maxDailyLoss         = 0.02;
    double maxExposure          = 5.0;
    int maxOpenOrders           = 10;
    int cooldownMs              = 250;
};

struct ExecConfig {
    int   minOrderIntervalMs = 5;
    int   maxOrdersPerSecond = 50;
    bool  enableBothSides    = true;
    bool  allowPartialFills  = true;
    std::string venue        = "BINANCE";
    bool useWebSocket        = true;
    int timeoutMs            = 5000;
    int retryCount           = 3;
    double slippageBps       = 5.0;
};

struct StrategySet {
    bool enableBase   = true;
    bool enableQ2     = true;
    bool enableHybrid = true;
    int hybridCount   = 8;
    double threshold  = 0.6;
};

struct EngineConfig {
    std::vector<std::string> symbols;
    std::string              instanceName;

    RiskConfig  riskConfig;
    ExecConfig  execConfig;
    StrategySet strategySet;
    
    std::string venueFIX;
    std::string venueBinance;

    std::function<void()>    yieldHook;
    std::function<std::uint64_t()> monotonicNow;
    std::function<std::uint64_t()> wallClockNow;

    std::string logPath = "./logs/";
    bool dryRun = false;
    int tickRateMs = 1;

    EngineConfig();
};

} // namespace Omega
