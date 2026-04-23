// =============================================================================
// gold_coordinator.cpp -- Gold engine coordinator implementation (SKELETON)
// =============================================================================
//
// See gold_coordinator.hpp for full design notes.
//
// SKELETON BEHAVIOR (Session 8, allow-by-default):
//   * can_enter() always returns true -- nothing is blocked.
//   * mark_entered() / mark_closed() update internal counters for diagnostics.
//   * No logging output from this file in the hot path -- diagnostics are
//     read via public accessors (position_count, reserved_lots, totals) from
//     wherever the caller wants to log them. Keeps this file side-effect free.
//
// NO ENGINE CALL SITES YET. Session 9+ wires tick_gold.hpp.
// =============================================================================

#include "gold_coordinator.hpp"

#include <algorithm>

namespace omega {

// -----------------------------------------------------------------------------
// Global instance
// -----------------------------------------------------------------------------
GoldCoordinator g_gold_coordinator;

// -----------------------------------------------------------------------------
// Construction
// -----------------------------------------------------------------------------
GoldCoordinator::GoldCoordinator() noexcept {
    // LaneState members self-initialise via in-class initialisers. Explicit
    // store() here would be redundant but harmless; leaving default-init in
    // place keeps the constructor trivially simple.
}

// -----------------------------------------------------------------------------
// can_enter -- SKELETON: always allow
// -----------------------------------------------------------------------------
bool GoldCoordinator::can_enter(GoldLane /*lane*/,
                                const char* /*symbol*/,
                                double /*lots*/) const noexcept
{
    // Session 8 skeleton: allow-by-default. Current tick_gold.hpp behaviour
    // is preserved. Session 9+ will introduce real budget-based denial by
    // comparing (reserved_lots + lots) against BUDGET_LOTS per lane.
    //
    // total_denied counter is NEVER incremented in the skeleton because we
    // never deny. When the real logic lands, denial paths will fetch_add
    // on lanes_[idx(lane)].total_denied.
    return true;
}

// -----------------------------------------------------------------------------
// mark_entered -- record, do not block
// -----------------------------------------------------------------------------
void GoldCoordinator::mark_entered(GoldLane lane,
                                   const char* /*symbol*/,
                                   const char* /*engine_name*/,
                                   double lots) noexcept
{
    const int i = idx(lane);
    if (i < 0 || i >= LANE_N) return;

    LaneState& ls = lanes_[i];
    ls.position_count.fetch_add(1, std::memory_order_relaxed);
    ls.total_entries.fetch_add(1, std::memory_order_relaxed);

    // Atomic add to reserved_lots. std::atomic<double> fetch_add is C++20 --
    // we're on /std:c++20 per CMakeLists.txt so this is valid. If the
    // compiler complains on any platform, the fallback is a compare_exchange
    // loop; not needed for MSVC 19.3x.
    ls.reserved_lots.fetch_add(lots, std::memory_order_relaxed);
}

// -----------------------------------------------------------------------------
// mark_closed -- record exit
// -----------------------------------------------------------------------------
void GoldCoordinator::mark_closed(GoldLane lane,
                                  const char* /*symbol*/,
                                  const char* /*engine_name*/,
                                  double lots) noexcept
{
    const int i = idx(lane);
    if (i < 0 || i >= LANE_N) return;

    LaneState& ls = lanes_[i];

    // Decrement position count but never below zero. Using compare_exchange
    // to avoid a negative count if mark_closed is ever called spuriously
    // (e.g. a stray exit log before the coordinator tracks the entry).
    int expected = ls.position_count.load(std::memory_order_relaxed);
    while (expected > 0) {
        if (ls.position_count.compare_exchange_weak(
                expected, expected - 1,
                std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            break;
        }
    }

    // Symmetric for reserved_lots -- never below zero.
    double expected_lots = ls.reserved_lots.load(std::memory_order_relaxed);
    while (expected_lots > 0.0) {
        const double new_lots = std::max(0.0, expected_lots - lots);
        if (ls.reserved_lots.compare_exchange_weak(
                expected_lots, new_lots,
                std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            break;
        }
    }

    ls.total_exits.fetch_add(1, std::memory_order_relaxed);
}

// -----------------------------------------------------------------------------
// budget_available -- SKELETON: always full budget
// -----------------------------------------------------------------------------
double GoldCoordinator::budget_available(GoldLane /*lane*/) const noexcept
{
    // Session 8 skeleton: returns full budget regardless of what is
    // currently marked reserved. Call sites that query this today will
    // always see headroom. Session 9+ will return
    //   max(0.0, BUDGET_LOTS - reserved_lots(lane))
    return BUDGET_LOTS;
}

// -----------------------------------------------------------------------------
// position_count -- diagnostic
// -----------------------------------------------------------------------------
int GoldCoordinator::position_count(GoldLane lane) const noexcept
{
    const int i = idx(lane);
    if (i < 0 || i >= LANE_N) return 0;
    return lanes_[i].position_count.load(std::memory_order_relaxed);
}

// -----------------------------------------------------------------------------
// reserved_lots -- diagnostic
// -----------------------------------------------------------------------------
double GoldCoordinator::reserved_lots(GoldLane lane) const noexcept
{
    const int i = idx(lane);
    if (i < 0 || i >= LANE_N) return 0.0;
    return lanes_[i].reserved_lots.load(std::memory_order_relaxed);
}

// -----------------------------------------------------------------------------
// Lifetime counters -- diagnostic
// -----------------------------------------------------------------------------
uint64_t GoldCoordinator::total_entries(GoldLane lane) const noexcept
{
    const int i = idx(lane);
    if (i < 0 || i >= LANE_N) return 0;
    return lanes_[i].total_entries.load(std::memory_order_relaxed);
}

uint64_t GoldCoordinator::total_exits(GoldLane lane) const noexcept
{
    const int i = idx(lane);
    if (i < 0 || i >= LANE_N) return 0;
    return lanes_[i].total_exits.load(std::memory_order_relaxed);
}

uint64_t GoldCoordinator::total_denied(GoldLane lane) const noexcept
{
    const int i = idx(lane);
    if (i < 0 || i >= LANE_N) return 0;
    return lanes_[i].total_denied.load(std::memory_order_relaxed);
}

// -----------------------------------------------------------------------------
// lane_name -- diagnostic helper
// -----------------------------------------------------------------------------
const char* GoldCoordinator::lane_name(GoldLane lane) noexcept
{
    switch (lane) {
        case GoldLane::BRACKET_LANE: return "BRACKET_LANE";
        case GoldLane::FLOW_LANE:    return "FLOW_LANE";
        case GoldLane::LANE_COUNT:   return "LANE_COUNT";
    }
    return "UNKNOWN_LANE";
}

} // namespace omega
