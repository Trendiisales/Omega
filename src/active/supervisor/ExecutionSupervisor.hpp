// =============================================================================
// ExecutionSupervisor.hpp - Execution Supervision (RESTORED)
// =============================================================================
#pragma once
#include <string>
#include <cmath>
#include "../execution/OrderIntent.hpp"
#include "../fix/execution/FIXExecHandler.hpp"
#include "../fix/execution/FIXReject.hpp"
#include "../engine/EngineConfig.hpp"

namespace Chimera {

class ExecutionSupervisor {
public:
    void init(const ExecConfig& cfg) noexcept;

    void setSymbol(const std::string& s) noexcept;
    void setMode(const std::string& m) noexcept;
    void setCoolDownMs(uint64_t ms) noexcept;
    void setMinConfidence(double c) noexcept;
    void setMaxPosition(double p) noexcept;

    bool approve(double confidence) noexcept;

    void route(const OrderIntent& intent) noexcept;

    void onExecution(const ExecReport& r) noexcept;
    void onReject(const FIXRejectInfo& r) noexcept;

private:
    std::string symbol;
    std::string mode;
    double maxPos = 1.0;
    double minConf = 0.01;
    uint64_t cooldownMs = 50;

    double currentPos = 0.0;
};

} // namespace Chimera
