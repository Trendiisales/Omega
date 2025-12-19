// =============================================================================
// CHIMERA_API_ASSERTS.hpp - COMPILE-TIME INTERFACE CHECKS
// =============================================================================
// If this file fails to compile, someone broke the locked API.
// Include this in main.cpp to enable checks.
// =============================================================================
#pragma once
#include <type_traits>
#include "CHIMERA_API_LOCK.hpp"

namespace CHIMERA_API_ASSERTS {

// ---- KillSwitch ----
static_assert(
    std::is_same_v<
        decltype(&Chimera::KillSwitch::trigger),
        void(*)() noexcept>,
    "KillSwitch::trigger signature mismatch");

static_assert(
    std::is_same_v<
        decltype(&Chimera::KillSwitch::clear),
        void(*)() noexcept>,
    "KillSwitch::clear signature mismatch");

static_assert(
    std::is_same_v<
        decltype(&Chimera::KillSwitch::isTriggered),
        bool(*)() noexcept>,
    "KillSwitch::isTriggered signature mismatch");

// ---- PnLTracker ----
static_assert(
    std::is_same_v<
        decltype(&Chimera::PnLTracker::onExec),
        void(Chimera::PnLTracker::*)(const Chimera::ExecReport&) noexcept>,
    "PnLTracker::onExec signature mismatch");

static_assert(
    std::is_same_v<
        decltype(&Chimera::PnLTracker::realized),
        double(Chimera::PnLTracker::*)() const noexcept>,
    "PnLTracker::realized signature mismatch");

static_assert(
    std::is_same_v<
        decltype(&Chimera::PnLTracker::fees),
        double(Chimera::PnLTracker::*)() const noexcept>,
    "PnLTracker::fees signature mismatch");

// ---- MicroMetrics POD verification ----
static_assert(
    std::is_standard_layout_v<Chimera::MicroMetrics>,
    "MicroMetrics must be standard layout");

// Note: RegimeClassifier::classify has overloads, cannot easily check with static_assert
// Compile will fail if signatures are wrong

} // namespace CHIMERA_API_ASSERTS
