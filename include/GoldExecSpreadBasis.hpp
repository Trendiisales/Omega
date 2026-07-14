#pragma once
// =============================================================================
// GoldExecSpreadBasis.hpp — the spread to feed XAUUSD ExecutionCostGuard calls
// (added S-2026-07-14, latent-class sweep P1-3 fix).
//
// Execution venue for the live gold desk is IBKR (COMEX GC/MGC futures book,
// port 4002) — the OmegaCostGuard XAUUSD row was rebased to that venue and its
// comment states "live bid-ask spread is fed in separately and is far tighter
// on the regulated futures book than the BlackBull spot markup". The live tick
// path's bid/ask, however, are still BlackBull FIX FEED quotes: passing
// (ask - bid) from them hands the guard a marked-up SPOT spread on top of a
// futures cost row — a phantom cost inflation that silently vetoes marginal
// entries (masked by the 1.5x hurdle + max_spread prefilters; no zero-trade
// symptom). GoldCostGateIBKR residue, systemic across the XAUUSD call-sites.
//
// Fix: XAUUSD is_viable calls pass this constant — 1 COMEX tick (0.10 pt), the
// typical GC book spread and the same basis the explicit MGC row documents
// ("live spread fed via spread_pts (typ 0.10)"). Feed-spread max_spread
// prefilters are untouched (they still veto a genuinely dislocated feed).
// MGC-instance engines whose bid/ask come from the real MGC book keep passing
// their live (ask - bid) — that IS the exchange spread.
//
// ONE definition on purpose: the 2026-07-14 latent-class sweep's failure class
// #1 is "second copies drift" — do not re-hardcode 0.10 at call-sites.
// =============================================================================
namespace omega {
inline constexpr double kGoldExecSpreadPts = 0.10;  // 1 COMEX GC tick, typical futures book spread
}
