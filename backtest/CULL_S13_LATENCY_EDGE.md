# Session 13 Finding B — LatencyEdgeStack Cull

## Status
**ACCEPTED.** `LatencyEdgeStack` + `GoldSpreadDislocation` +
`GoldEventCompression` removed from the build. The stack was already
self-disabled in code (see evidence below) but the class, instance,
config fields, config-file section, enum value, and 11 call-sites
remained in the codebase.

Repo HEAD at cull: `3955e125` (post S13 orphan cull, pre-Finding B).
Decision trigger: Session 13 engine audit.

## Evidence — the stack was already a no-op

`include/LatencyEdgeEngines.hpp` contained this body since an earlier
session (commit predating S13):

```cpp
// LatencyEdgeStack disabled: VPS RTT ~68ms, latency edge requires <1ms.
// No positions can be opened or exist. All calls are no-ops.
// To re-enable: remove this body and restore per-engine on_tick calls.
LeSignal on_tick_gold(double /*bid*/, double /*ask*/, double /*latency_ms*/,
                      CloseCb /*on_close*/, bool /*can_enter*/ = true) noexcept {
    return {};
}
bool has_open_position() const noexcept { return false; }
void force_close_all(...) noexcept { /* no-op */ }
```

The only remaining public behaviour was `print_stats()` emitting
`[LATENCY-EDGE] SpreadDisloc trades=0 EventComp trades=0` every 30s.

The Session 13 audit confirmed zero LatencyEdge trades across the 8-day
S17 live window (`backtest/audit_results/engine_summary.csv`: no
GoldSpreadDisloc / GoldEventComp / LatencyEdge rows).

## Why not re-enable instead of cull?

Per the header comment, re-enabling requires VPS RTT < 1ms. Current VPS
(185.167.119.59) runs at ~68ms RTT. Achieving <1ms requires physical
co-location at the broker's matching engine — not a software change.
Until that infrastructure shift happens, the engine cannot ever fire.

Deletion is reversible: git history preserves the full class hierarchy.
If a colocated VPS is acquired, the engine can be restored from the
pre-cull commit.

## Changes in this cull

### File deleted
- `include/LatencyEdgeEngines.hpp` (30,716 bytes)

### Files modified (all 12, full-file replacements — no diffs, no snippets)

1. **`include/globals.hpp`** — removed `static omega::latency::LatencyEdgeStack g_le_stack;` declaration. Replaced with 3-line tombstone comment.

2. **`src/main.cpp`** — removed `#include "LatencyEdgeEngines.hpp"`. Replaced with 1-line tombstone comment.

3. **`include/omega_types.hpp`** — removed `omega::latency::LatencyEdgeCfg le_cfg;` field from config struct + its 1-line header comment. Replaced with 1-line tombstone.

4. **`include/engine_config.hpp`** — removed 17-line `[latency_edge]` ini-parser block. Replaced with 2-line tombstone. `[latency_edge]` keys now silently ignored if present in ini files.

5. **`include/engine_init.hpp`** — removed `g_le_stack.configure(g_cfg.le_cfg);` call and associated comment block (4 patches in the file). This commit also carries the **Finding A MacroCrash threshold revert** (3-line config change) — see `backtest/CULL_S13_MCE_REVERT.md`.

6. **`include/tick_gold.hpp`** — removed 3 active `g_le_stack.has_open_position()` calls from `gold_any_open` + `trend_pb_gold_can_enter` + `hybrid_can_enter` predicates. Removed 2 comment refs. Replaced each with tombstone comments.

7. **`include/trade_lifecycle.hpp`** — removed 2 `static_cast<int>(g_le_stack.has_open_position())` terms from position-count summations. Replaced each with `0 + // tombstone` so sum arithmetic is preserved.

8. **`include/quote_loop.hpp`** — removed `g_le_stack.print_stats()` from 30s stats block and 4 `g_le_stack.force_close_all(...)` call sites (stale-reconnect, shutdown, daily-close, live-reconnect). Replaced with tombstone comments.

9. **`include/config.hpp`** — removed midnight-rollover `force_close_all` block (7 lines).

10. **`include/OmegaRegimeAdaptor.hpp`** — removed `LATENCY_EDGE` enum value and associated `RegimeWeightTable` row.

11. **`include/gold_coordinator.hpp`** — removed `LatencyEdge` mention from a comment enumerating gold engines.

12. **`omega_config.ini`** — removed `[latency_edge]` section (19 lines) — no longer parsed anywhere.

### Net codebase change
- `-30,716` bytes from deleted header
- `-1,519` bytes from engine_config.hpp parser block
- `-1,836` bytes from quote_loop.hpp call sites
- `-1,159` bytes from omega_config.ini section
- `-324` bytes from config.hpp midnight block
- `-30` bytes from omega_types.hpp field
- Small positive deltas from tombstone comments in remaining 6 files
- **Approximate net: -35 KB across 13 files** (12 modified + 1 deleted)

## Risk assessment

**Zero runtime risk.** The stack's `on_tick_gold()` returned `{}`,
`has_open_position()` returned `false`, `force_close_all()` was a
no-op. Removing these calls changes nothing the engine would have done.

**Build risk: zero.** Every call site was removed with same-signature
tombstone (comment), except `trade_lifecycle.hpp` which needed a `0 +`
placeholder to preserve sum-of-open-positions arithmetic.

**Config compatibility: preserved.** If an old `omega_config.ini` with
a `[latency_edge]` section is deployed, the new parser simply ignores
unknown section keys (standard INI parsing behaviour in this codebase,
same pattern used for GoldSilverLeadLag which was deleted earlier).

## Verification post-merge

1. `QUICK_RESTART.ps1` on VPS — expect clean MSVC build.
2. `VERIFY_STARTUP.ps1` — expect same pass/fail profile as pre-cull
   (the LATENCY-EDGE stats line will no longer appear in log).
3. Log grep — `grep "LATENCY-EDGE\|LE-CFG\|LE-CB"` in post-startup log
   should return zero results.

## Follow-up for future sessions

If VPS is ever moved to colocation:
1. Restore `include/LatencyEdgeEngines.hpp` from git history
   (last version: commit predating `3955e125`).
2. Revert all 12 files from this cull's tombstone comments
   (search repo for `S13 Finding B 2026-04-24`).
3. Revive `[latency_edge]` config section.
4. Re-enable `on_tick_gold()` body.

---

*Document generated 2026-04-24, Session 13 Stage 2, Claude.*
*Repo HEAD at cull: `3955e125` → new HEAD after this commit.*
