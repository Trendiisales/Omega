#pragma once
// =============================================================================
// OmegaCostGate.hpp -- Thin inversion wrapper around ExecutionCostGuard::is_viable
// =============================================================================
//
// 2026-05-12 (Claude / Jo): ExecutionCostGuard::is_viable already exists in
//   OmegaCostGuard.hpp lines 100-117. This file just provides a `should_skip`
//   convenience wrapper for engines whose call-site reads more naturally as
//   "skip if cost gate triggers" than "fire if viable".
//
//   Use whichever feels natural at the call site. Both delegate to the same
//   estimated_cost_usd + expected_gross_usd primitives.
//
// USAGE
//   #include "OmegaCostGate.hpp"          // brings OmegaCostGuard.hpp with it
//   if (omega::cost_gate::should_skip("GBPUSD", spread, tp_dist, base_lot)) {
//       phase = Phase::IDLE; return;
//   }
// =============================================================================

#include "OmegaCostGuard.hpp"

namespace omega { namespace cost_gate {

inline bool should_skip(const char* sym,
                        double spread_pts,
                        double tp_dist_pts,
                        double lot,
                        double cost_ratio_min = 1.5) noexcept
{
    return !ExecutionCostGuard::is_viable(sym, spread_pts, tp_dist_pts, lot,
                                          cost_ratio_min);
}

} } // namespace omega::cost_gate
