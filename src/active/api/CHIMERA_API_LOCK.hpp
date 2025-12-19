// =============================================================================
// CHIMERA_API_LOCK.hpp - LOCKED PUBLIC API SURFACE
// =============================================================================
// DO NOT MODIFY THIS FILE.
//
// This file defines the locked public API surface of CHIMERA_HFT.
// Any change here REQUIRES:
//   - Architecture review
//   - Version bump
//   - Regeneration of audit artifacts
// =============================================================================
#pragma once

#include "../pipeline/MicroMetrics.hpp"
#include "../fix/execution/FIXExecHandler.hpp"
#include "../execution/OrderIntent.hpp"
#include "../risk/KillSwitch.hpp"
#include "../risk/RegimeClassifier.hpp"
#include "../positions/PnLTracker.hpp"
#include "../supervisor/ExecutionSupervisor.hpp"

namespace CHIMERA_API {

// ============================================================================
// LOCKED INTERFACES - DO NOT MODIFY SIGNATURES
// ============================================================================

// ---- KillSwitch (static) ----
// void trigger() noexcept;
// void clear() noexcept;
// bool isTriggered() noexcept;

// ---- RegimeClassifier ----
// static Regime classify(const MicroMetrics& m) noexcept;

// ---- PnLTracker ----
// void onExec(const ExecReport& r) noexcept;
// double realized() const noexcept;
// double fees() const noexcept;

// ---- ExecutionSupervisor ----
// void init(const ExecConfig& cfg) noexcept;
// void setSymbol(const std::string& s) noexcept;
// void setMode(const std::string& m) noexcept;
// void setCoolDownMs(uint64_t ms) noexcept;
// void setMinConfidence(double c) noexcept;
// void setMaxPosition(double p) noexcept;
// bool approve(double confidence) noexcept;
// void route(const OrderIntent& intent) noexcept;
// void onExecution(const ExecReport& r) noexcept;
// void onReject(const FIXRejectInfo& r) noexcept;

// ---- MicroMetrics (POD) ----
// bool   shockFlag;
// double trendScore;
// double volRatio;
// double lastMid;
// double emaMid;
// double emaVol;
// uint64_t tickCount;

// ---- TickPipelineExt ----
// void init(const std::string& symbol) noexcept;
// void pushTick(const Tick& t) noexcept;
// void pushBook(const OrderBook& b) noexcept;
// bool compute(MicroMetrics& out) noexcept;
// void computeBook(MicroMetrics& out) noexcept;

} // namespace CHIMERA_API
