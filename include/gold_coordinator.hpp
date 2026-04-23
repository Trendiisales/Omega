// =============================================================================
// gold_coordinator.hpp -- Gold engine coordinator (SKELETON)
// =============================================================================
//
// PURPOSE
// -------
// Replaces the per-engine exclusivity cascade in tick_gold.hpp with a single
// global coordinator that tracks two lanes of gold engines:
//
//   BRACKET_LANE  -- structural/bracket engines (6 engines, Session 9 migration):
//                    BracketGold, TrendBracket, HybridBracket, CompBreakout,
//                    EMACross, BBMeanRev
//
//   FLOW_LANE     -- flow/directional engines (remaining engines, later sessions):
//                    GoldFlow, GoldFlowReload, GoldFlowAddon, GoldStack,
//                    CandleFlow, MacroCrash, RSIReversal,
//                    RSIExtremeTurn, PullbackCont, PullbackPrem, TrendPullback,
//                    LatencyEdge, NBMLondon, H1Swing, H4Regime, PDHLRev
//                    (DomPersist removed at Session 15 2026-04-23 -- no edge.)
//
// Budget per lane: BUDGET_LOTS = 0.01 (skeleton default -- tunable later).
//
// DESIGN
// ------
// * Lock-free hot path: all lane state is in std::atomic -- no mutex in the
//   tick handler. Matches OMEGA arch rule (no mutex hot path).
// * Engine-style API: can_enter() / mark_entered() / mark_closed() matches the
//   existing has_open_position() pattern used throughout tick_gold.hpp.
// * Session 8 skeleton = ALLOW-BY-DEFAULT (Option B):
//     can_enter()        -> true
//     mark_entered()     -> records internally, does not block
//     mark_closed()      -> clears internal tracking
//     budget_available() -> returns BUDGET_LOTS
//   Current tick_gold.hpp behavior is unchanged until Session 9 flips the
//   blocking logic on per-engine.
// * Internal state IS tracked even in skeleton mode so diagnostics can verify
//   the coordinator sees entries/exits correctly before we rely on it.
//
// CONSTRUCTION
// ------------
// A single global instance g_gold_coordinator is defined in gold_coordinator.cpp
// and declared extern here. Same pattern as g_gold_flow, g_gold_stack, etc.
//
// THREAD SAFETY
// -------------
// All methods are safe to call from any thread. Per-lane position counters
// are std::atomic<int>; per-lane reserved-lots are std::atomic<double> via
// fetch_add/fetch_sub on a uint64_t bit pattern. Skeleton uses a simple
// std::atomic<int> count because no real lots enforcement happens yet.
//
// NO CALL SITES IN THIS SKELETON. Session 9+ wires engines to call these
// methods; this file simply declares the class and provides the stub.
// =============================================================================

#pragma once

#include <atomic>
#include <cstdint>

namespace omega {

// -----------------------------------------------------------------------------
// Lane enumeration
// -----------------------------------------------------------------------------
enum class GoldLane : int {
    BRACKET_LANE = 0,
    FLOW_LANE    = 1,
    LANE_COUNT   = 2   // sentinel for array sizing
};

// -----------------------------------------------------------------------------
// Coordinator class (skeleton -- allow-by-default)
// -----------------------------------------------------------------------------
class GoldCoordinator {
public:
    // Per-lane lot budget. Skeleton value; Session 10+ may make per-lane.
    static constexpr double BUDGET_LOTS = 0.01;

    GoldCoordinator() noexcept;

    // ---------------------------------------------------------------------
    // ENGINE-STYLE API -- matches has_open_position() pattern
    // ---------------------------------------------------------------------
    //
    // can_enter(lane, symbol, lots)
    //   Returns true if an engine on `lane` may open a new position of size
    //   `lots` on `symbol`. SKELETON: always returns true. Session 9 will
    //   flip this to check per-lane budget.
    //
    // mark_entered(lane, symbol, engine_name, lots)
    //   Record that an engine has opened a position. SKELETON: increments
    //   internal counter for diagnostics. Does not block anything.
    //
    // mark_closed(lane, symbol, engine_name, lots)
    //   Record that an engine has closed a position. SKELETON: decrements
    //   internal counter.
    //
    // budget_available(lane)
    //   Returns lots still available on this lane. SKELETON: always
    //   returns BUDGET_LOTS (i.e. full budget).
    //
    // position_count(lane)
    //   Diagnostic: how many positions the coordinator thinks are open on
    //   this lane. Reflects mark_entered/mark_closed calls made so far.
    //
    // reserved_lots(lane)
    //   Diagnostic: sum of lots currently reserved on this lane per the
    //   coordinator's internal tracking.
    // ---------------------------------------------------------------------

    bool   can_enter(GoldLane lane, const char* symbol, double lots) const noexcept;
    void   mark_entered(GoldLane lane, const char* symbol, const char* engine_name, double lots) noexcept;
    void   mark_closed (GoldLane lane, const char* symbol, const char* engine_name, double lots) noexcept;

    double budget_available(GoldLane lane) const noexcept;
    int    position_count  (GoldLane lane) const noexcept;
    double reserved_lots   (GoldLane lane) const noexcept;

    // ---------------------------------------------------------------------
    // Diagnostics -- lifetime counters, never reset
    // ---------------------------------------------------------------------
    //
    // total_entries / total_exits: every mark_entered / mark_closed call.
    // total_denied:                can_enter() returning false (always 0 in
    //                              skeleton since can_enter() never denies).
    // ---------------------------------------------------------------------

    uint64_t total_entries(GoldLane lane) const noexcept;
    uint64_t total_exits  (GoldLane lane) const noexcept;
    uint64_t total_denied (GoldLane lane) const noexcept;

    // ---------------------------------------------------------------------
    // Helpers
    // ---------------------------------------------------------------------
    static const char* lane_name(GoldLane lane) noexcept;

private:
    // Per-lane state. Plain std::atomic<int> / <double>. No padding games in
    // the skeleton -- hot-path concurrency will be reviewed again when the
    // coordinator actually gates entries (Session 9+).
    struct LaneState {
        std::atomic<int>      position_count{0};
        std::atomic<double>   reserved_lots{0.0};
        std::atomic<uint64_t> total_entries{0};
        std::atomic<uint64_t> total_exits{0};
        std::atomic<uint64_t> total_denied{0};
    };

    static constexpr int LANE_N = static_cast<int>(GoldLane::LANE_COUNT);
    LaneState lanes_[LANE_N];

    // Index helper. Bounds-checked only in debug; release is a direct cast.
    static inline int idx(GoldLane lane) noexcept {
        return static_cast<int>(lane);
    }
};

// -----------------------------------------------------------------------------
// Global instance -- defined in gold_coordinator.cpp
// -----------------------------------------------------------------------------
extern GoldCoordinator g_gold_coordinator;

} // namespace omega
