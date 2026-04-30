# Omega Session Handoff — 2026-04-30c (CellEngine Phase 1)

**Final State:** Phase 1 code complete; commit pending user action via
`COMMIT_PHASE1.sh`. Production VPS still running commit `1db4408` unchanged.

**Parent commit assumed:** `audit-fixes-31` (= what
`COMMIT_SESSION_2026-04-30b.sh` produces). If that hasn't landed yet, run
that script first or this commit will stack onto whatever HEAD is.

---

## What shipped this session

### CellEngine refactor — Phase 1 (extract shared primitives)

Pure additive change. **No engine behaviour change.** Phase 1 introduces the
canonical shared types that Phase 2-3 will migrate engines to use, plus
compile-time structural-sanity asserts in each engine that lock layout
compatibility going forward.

**New file: `include/CellPrimitives.hpp`** (~210 lines)

Canonical types in `namespace omega::cell`:

| Type | Size | Purpose |
|---|---:|---|
| `Bar` | 40 B | OHLC bar; identical to `TsmomBar` / `DonchianBar` / `EpbBar` / `TrBar` |
| `BarSynth` | 48 B | H1 → H2/H4/H6/D1 aggregator; identical aggregation logic to all four |
| `ATR14` | 32 B | Wilder ATR14 with same math as the four private copies |
| `EMA` | 40 B | `pandas.ewm(span,adjust=False)` reproduction; matches `EpbEMA` |
| `Position` | 96 B | NEW shape — adds `initial_sl` (for Phase 3 stage trail) + `tp` (for Donchian/Epb migration) |
| `SizingParams` | 16 B | Forward-decl for Phase 2 `Strategy::sizing()` hook |

`Position` intentionally diverges from `TsmomCell::Position` by adding two
fields. This is not used by any engine in Phase 1 — it's defined now so
Phase 2 can review the shape without needing a separate edit. The Phase 2
byte-level ledger validation catches any drift before V1 retires.

**Modified files:** `include/{Tsmom,Donchian,EmaPullback,TrendRider}Engine.hpp`

Each engine header now:

1. `#include "CellPrimitives.hpp"` immediately after `#include "OmegaTradeLedger.hpp"`.
2. After its private helper struct definitions (`XBar`, `XBarSynth`, `XATR14`,
   plus `EpbEMA` for EmaPullback), declares a `static_assert` block confirming:

   ```cpp
   static_assert(sizeof(TsmomBar)      == sizeof(::omega::cell::Bar),      "...");
   static_assert(sizeof(TsmomBarSynth) == sizeof(::omega::cell::BarSynth), "...");
   static_assert(sizeof(TsmomATR14)    == sizeof(::omega::cell::ATR14),    "...");
   ```

   EmaPullback gets a fourth assert for `EpbEMA == omega::cell::EMA`.

Total: 13 static_asserts (3 each in Tsmom/Donchian/TrendRider + 4 in
EmaPullback). All pass at compile time; any future drift becomes a hard
compile error pointing at the exact mismatched type.

---

## Why this is structured this way

The plan (§4 Phase 1) called for shared primitives **and** static_asserts
in each engine. The two pieces are intentionally coupled:

- The `omega::cell` types alone would just sit unused. Phase 2 needs them
  but no Phase 1 caller needs them.
- The static_asserts alone would have nothing canonical to compare against.
- Together they form a structural-sanity contract: any change to any
  engine's private helper struct (or to the canonical type) that would
  break layout-equivalence becomes a compile failure now, not a
  shadow-ledger surprise in Phase 2.

This is the cheapest possible bug-prevention against the Phase 2 risk
("V2 produces different trades than V1") — and it also gives us a place
to catch the duplication-driven bug class the plan §2.4 calls out.

---

## Diff totals

```
 include/CellPrimitives.hpp              | +210 / -0  (new file)
 include/TsmomEngine.hpp                 | +16  / -0
 include/DonchianEngine.hpp              | +16  / -0
 include/EmaPullbackEngine.hpp           | +18  / -0
 include/TrendRiderEngine.hpp            | +16  / -0
 docs/SESSION_2026-04-30c_PHASE1_HANDOFF.md | new file
 COMMIT_PHASE1.sh                        | new file (helper, not committed itself)
```

Engine-header changes total: 66 insertions, 0 deletions across 4 .hpp files.
Plus the new ~210-line CellPrimitives.hpp.

---

## Verification at this point

- `g++ -std=c++20 -Wall -Wextra -Wpedantic -Werror -O2` standalone test
  driver (`#include "CellPrimitives.hpp"` + size/POD asserts): **PASS**
- `g++ -std=c++20 -fsyntax-only` on each of the 4 modified engine headers
  (with the new `#include` resolved against `-I include`): **PASS** —
  meaning all 13 static_asserts hold.
- Sizes confirmed: `Bar=40`, `BarSynth=48`, `ATR14=32`, `EMA=40`,
  `Position=96`.
- Full Windows MSVC build verification: **NOT RUN** — sandbox can't
  replicate the toolchain. Must run on VPS via `QUICK_RESTART.ps1` before
  deploy.

---

## Verification still required (user action)

1. **If `audit-fixes-31` not yet committed:**
   `bash COMMIT_SESSION_2026-04-30b.sh` first.
2. `bash COMMIT_PHASE1.sh` from your Mac terminal.
3. `git push origin main` (you'll be 10 commits ahead pre-push if both
   commits land; 9 if you push them in sequence).
4. Build on Windows VPS via `QUICK_RESTART.ps1`. The MSVC build is the
   real test — g++ accepts a few things MSVC does not, and vice versa.
5. Confirm runtime behaviour unchanged in next 24h of trade ledger:
   - All 4 cell-engine portfolios still warm up identically (same
     synth-bar counts: H2=3078 / H4=1539 / H6=1026 / D1=256).
   - All 26 profitable cells still firing. (Phase 1 does not touch
     dispatch; if any cell goes silent that's a real bug.)

---

## What Phase 1 explicitly does NOT do

- Does not introduce `CellEngine.hpp` (= `CellBase<Strategy>` template).
  That's Phase 2.
- Does not migrate any engine to use the canonical types at runtime.
  Engines still operate on their own `XBar`/`XBarSynth`/`XATR14`/`EpbEMA`.
- Does not touch dispatch, sizing, or any cell logic.
- Does not introduce the C++20 `concept CellStrategy` from §7.2. That
  belongs with `CellEngine.hpp` in Phase 2.

If a runtime divergence appears after this commit deploys, **it is not
Phase 1** — Phase 1 cannot reach the runtime. Most likely culprit would
be the prior `audit-fixes-31` ship landing concurrently. Roll Phase 1
back independently if needed: it touches no logic.

---

## Deferred / not done this session

- **VPS install/deploy issues:** still the top priority. Phase 1 ships
  no production change but Phase 2-3 will, and without working deploys
  refactor commits accumulate unbuilt. Diagnose at next session start.
- **Phase 2 (`CellEngine.hpp` + `TsmomStrategy` + V1/V2 shadow):**
  queued. Per plan §4 Phase 2, this is the highest-risk phase — full
  byte-level ledger comparison required before V1 retirement.
- **Push to origin:** repo is now 10 commits ahead of origin/main if the
  prior session's commit also lands. User decides when to push.

---

## Open / known issues flagged

### VPS install/deploy failures
Still unresolved as of Phase 1 ship. Without a working deploy pipeline,
Phase 1 + Phase 2 + Phase 3 commits stack up without ever reaching
production. Recommend treating this as a hard blocker for Phase 2 — no
point landing more refactor code if we can't validate it under live
conditions.

### Stale `.git/index.lock`
`COMMIT_PHASE1.sh` removes it at Step 1, same pattern as
`COMMIT_SESSION_2026-04-30b.sh`.

---

## Watch in next session

- Phase 1 commit visible at `git log` with title
  `cell-refactor-phase-1: extract shared primitives + structural sanity asserts`
- After Windows build: no new symbols exposed to runtime (Phase 1 is
  header-only, additive). Build artifact size should be functionally
  unchanged (the static_asserts compile to nothing).
- VPS deploy pipeline status (real top item).

---

## Next session agenda (queued)

Order matters — the VPS issue still blocks production validation.

1. **Resolve VPS install/deploy issues** (still TOP PRIORITY).
2. **Phase 2: introduce `include/CellEngine.hpp`** with `CellBase<Strategy>`
   and `CellPortfolio<Strategy>`. Implement `TsmomStrategy`. Add parallel
   `g_tsmom_v2` instance running shadow alongside `g_tsmom`.
3. **Phase 2a validation:** V1 vs V2 with `max_positions_per_cell=1` for
   both — must produce byte-identical ledgers for at least 5 trading days
   before any further change.
4. **Phase 2b rollout:** flip V2 to `max_positions_per_cell=10` (the §7.1
   policy change). Observe new V2 trade behaviour for 5 more days before
   retiring V1.

Phase 3 (Donchian + EmaPullback + TrendRider strategies) is the session
after Phase 2 lands cleanly.

---

## How this session built on the prior 2026-04-30b ship

Prior session shipped the GoldStack sub-engine audit-disables (Wave 2),
the NAS100 hybrid bracket session tightening (slot 3-4 only), and the
CellEngine refactor PLAN. The §6.3 timing override locked Phase 1 to
proceed immediately at next session — this session executed that. Plan
itself is unchanged; this session is the first code landing under it.

The audit-disables and the refactor are independent: they touch
different files (`GoldEngineStack.hpp`/`engine_init.hpp`/`globals.hpp`/
`tick_indices.hpp` vs. the four cell engine headers + new
`CellPrimitives.hpp`). Either commit can roll back without affecting
the other.
