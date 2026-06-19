#pragma once
// =============================================================================
// PortfolioGovernor.hpp -- correlation-aware cross-symbol entry governor
// (S-2026-06-19 Phase 1 item 2)
// =============================================================================
//
// WHAT THIS IS (and is NOT):
//   This is the CROSS-SYMBOL, correlation-group campaign cap. It is distinct
//   from PortfolioGuard.hpp (omega::pg) which is a GOLD-ONLY concurrency +
//   sizing + kill-file overlay for the XAU trend zoo. The governor sits in
//   symbol_gate (trade_lifecycle.hpp) and decides whether a NEW position on a
//   given symbol may open given what is ALREADY open across the whole book.
//
// THE RULES (handoff item 2):
//   1. >=1 campaign per symbol            -- already enforced by symbol_gate's
//      `if (symbol_has_open_position) return false` at the top of the gate.
//      The governor therefore only reasons about OTHER symbols.
//   2. max 1 campaign per correlation group -- NAS100/US500/USTEC/DJ30 are one
//      US_EQUITY group; opening a 2nd US-index while one is live = correlated
//      stacking (a single risk-off reversal hits both). Blocked.
//   3. max N total campaigns (default 2)  -- global cap on distinct-symbol
//      campaigns across the book.
//   4. no same-direction stacking         -- SUBSUMED by rule 2 when
//      max_per_corr_group == 1 (you cannot have a 2nd position in the group at
//      all, same direction or not). Kept as a named cfg toggle for the future
//      case where a group cap > 1 is allowed.
//
// WHY STATIC GROUPS (not the live OmegaCorrelationMatrix):
//   The EWM correlation gate was REMOVED from the entry path (false blocks with
//   1-2 open positions; see symbol_gate comment). Static instrument-class
//   groups are robust and need no warmup. NAS/SPX/US30 ARE correlated every
//   day -- no need to measure it.
//
// SOURCE OF TRUTH:
//   g_open_positions.snapshot_all() -- the live registry (each engine's
//   collect_positions lambda; mutex-guarded; built fresh per call). Coverage is
//   broad (102 sources incl. all index engines) but not provably 100%. A gap =
//   UNDER-enforcement = no worse than today (the governor only ever BLOCKS, it
//   never forces an entry), so it is fail-safe.
//
// COST:
//   snapshot_all() walks ~100 lambdas under a mutex. The caller gates this
//   behind `open_positions > 0` (the cheap int symbol_gate already computes) so
//   the snapshot only runs when a second concurrent campaign is even possible.
// =============================================================================

#include <string>
#include <vector>
#include <set>
#include "OpenPositionRegistry.hpp"   // omega::PositionSnapshot, g_open_positions

namespace omega {

enum class CorrGroup { US_EQUITY, EU_EQUITY, METALS, OIL, FX_USD, OTHER };

// Map a broker symbol to its static correlation group. OTHER = uncorrelated
// (no group cap applies; only the global total-campaign cap does).
inline CorrGroup corr_group_of(const std::string& s) noexcept
{
    if (s == "US500.F" || s == "USTEC.F" || s == "DJ30.F" || s == "NAS100")
        return CorrGroup::US_EQUITY;
    if (s == "GER40" || s == "UK100" || s == "ESTX50")
        return CorrGroup::EU_EQUITY;
    if (s == "XAUUSD" || s == "XAGUSD")
        return CorrGroup::METALS;
    if (s == "USOIL.F" || s == "BRENT")
        return CorrGroup::OIL;
    if (s == "EURUSD" || s == "GBPUSD" || s == "AUDUSD" ||
        s == "NZDUSD" || s == "USDJPY")
        return CorrGroup::FX_USD;
    return CorrGroup::OTHER;
}

inline const char* corr_group_name(CorrGroup g) noexcept
{
    switch (g) {
        case CorrGroup::US_EQUITY: return "US_EQUITY";
        case CorrGroup::EU_EQUITY: return "EU_EQUITY";
        case CorrGroup::METALS:    return "METALS";
        case CorrGroup::OIL:       return "OIL";
        case CorrGroup::FX_USD:    return "FX_USD";
        default:                   return "OTHER";
    }
}

struct PortfolioGovernorCfg {
    bool enabled              = false;  // master switch (ini: portfolio_governor_enabled)
    int  max_total_campaigns  = 2;      // global cap on distinct-symbol campaigns
    int  max_per_corr_group   = 1;      // 1 correlated index/equity campaign
    bool block_same_dir_stack = true;   // only bites when max_per_corr_group > 1
};

struct GovernorVerdict {
    bool        allow  = true;
    const char* reason = "";   // "max_total" | "corr_group" -- for the block log
};

// Pure decision. `open` is the live snapshot; `cand_sym` is the symbol whose
// new entry is being considered. Same-symbol positions are ignored here (the
// per-symbol single-campaign rule lives in symbol_gate). Direction-free: with
// max_per_corr_group == 1 the same-direction rule is automatically satisfied.
inline GovernorVerdict governor_check(
    const PortfolioGovernorCfg& cfg,
    const std::vector<PositionSnapshot>& open,
    const std::string& cand_sym) noexcept
{
    if (!cfg.enabled) return {true, ""};

    const CorrGroup cand_group = corr_group_of(cand_sym);
    std::set<std::string> campaign_syms;   // distinct OTHER symbols with a position
    int group_count = 0;

    for (const auto& p : open) {
        if (p.symbol == cand_sym) continue;          // same-symbol -> symbol_gate's job
        campaign_syms.insert(p.symbol);
        if (cand_group != CorrGroup::OTHER &&
            corr_group_of(p.symbol) == cand_group)
            ++group_count;
    }

    if (static_cast<int>(campaign_syms.size()) >= cfg.max_total_campaigns)
        return {false, "max_total"};

    if (cand_group != CorrGroup::OTHER && group_count >= cfg.max_per_corr_group)
        return {false, "corr_group"};

    return {true, ""};
}

} // namespace omega
