# Session 13 — Orphan Header & Backup File Cull

## Status
**ACCEPTED.** Five engine header files exist in the repo but are **never
#include'd and never instantiated**. They compile into nothing. Plus 22
stale `.bak*` files totalling 3.63 MB of dead backups remain in the
repo.

Repo HEAD at cull: `850e8fa6`.

## Methodology
Exhaustive local grep across all 81 production source files (81 hpp/cpp
files under `include/`, `src/`, and `src/engines/`, excluding `backtest/`
and all `.bak*` files). Verified with `grep -l <ClassName> all_src/*`
that each orphan class name appears **only in its own header file**,
with no #include statements and no instantiation elsewhere.

Three secondary comment references exist (see Tier 1 below) but produce
no compiled code.

## Tier 1 — Orphan engine headers (5 files, 43,018 bytes)

Dead code. Never wired. Safe to delete.

| File | Size | Non-self references |
|------|------|---------------------|
| `include/StopRunReversalEngine.hpp`   | 12,045 B | none |
| `include/MomentumBreakoutEngine.hpp`  | 13,075 B | One *comment* in `include/RealDomReceiver.hpp:11` ("read by CandleFlowEngine/MomentumBreakoutEngine" — CFE is culled, MBE was never wired) |
| `include/OverlapFadeEngine.hpp`       |  6,494 B | Two *comments*: `include/omega/types.hpp:79`, `include/tick/gold.hpp:1311` (both future-plan notes) |
| `include/StructuralEdgeEngines.hpp`   | 10,153 B | none |
| `src/engines/MacroImpulseEngine.hpp`  |  1,251 B | none |

### Action
Delete the 5 files. Remove the 3 comment references (comments describe
engines that do not exist in the live build; they confuse future audits).

### Post-deletion verification
After deletion, `grep -rE "StopRunReversalEngine\|MomentumBreakoutEngine\|OverlapFadeEngine\|StructuralEdge\|MacroImpulseEngine" include/ src/` should return zero results.

## Tier 2 — Stale backup files (22 files, 3,802,756 bytes = 3.63 MB)

Git preserves all history. These in-tree backups are pure bloat.

### GoldEngineStack backups (3 files, 620 KB)
- `include/GoldEngineStack.hpp.bak` — 107,509 B
- `include/GoldEngineStack.hpp.bak_20260422_061125` — 256,529 B
- `include/GoldEngineStack.hpp.bak_20260422_061324` — 256,529 B

### GoldFlowEngine backups (17 files, 2.3 MB)
All `include/GoldFlowEngine.hpp.bak_*` files. Engine was culled
S19 Stage 1B — these backups predate the cull.

- `GoldFlowEngine.hpp.bak_20260404_183911` through `_20260404_204631` (11 files)
- `GoldFlowEngine.hpp.bak_202813`, `_203545`, `_203629`, `_204720`, `_205321`, `_205432` (6 files)

### main.cpp backups (2 files, 881 KB)
- `src/main.cpp.bak` — 39,446 B
- `src/main.cpp.bak_20260405_080311` — 841,452 B  ← biggest single cull

### Action
`git rm` all 22 files in a single commit.

## Risk assessment

**Zero risk of compilation/runtime regression.**

1. Orphan headers: never included, never instantiated — deletion cannot
   break any build dependency. Verified via exhaustive grep.
2. Backup files: by naming convention (`.bak`, `.bak_<timestamp>`) they
   were created as manual snapshots; they are not part of the build.
   CMakeLists.txt does not reference any `.bak` file.
3. Comments referencing deleted engines: deleting these comment lines
   only affects developer reading, not behaviour.

**Verification after merge:**
1. `git rm` commit → push
2. Run `QUICK_RESTART.ps1` on VPS — confirms clean MSVC build
3. `VERIFY_STARTUP.ps1` should pass with same pass/fail profile as
   before the cull (the L2 FAIL is a verifier false-positive; GF-CONFIG
   FAIL disappears when verifier is updated in a separate commit)

## Implications for future work

None of these cull targets overlap with the queued Tier 2 re-validations
or Tier 4 substrategy sweeps. The production dispatch roster on XAUUSD
is unchanged. This is pure hygiene.

## Separate findings for follow-up (NOT part of this cull)

These emerged during the audit but require live-data decisions, not
immediate cull:

1. **g_bracket_gold vs g_hybrid_gold overlap.** Both are bracket engines
   on XAUUSD. S17 audit shows `g_bracket_gold` (XAUUSD_BRACKET) at 33
   trades / 12.1% WR / -$3.80/trade, vs `g_hybrid_gold` (HybridBracketGold)
   at 12 trades / 41.7% WR / -$0.57/trade. Hybrid is 3.5× better WR and
   6.5× lower per-trade cost. Strong candidate for S14+ decision:
   either disable `g_bracket_gold` in favour of `g_hybrid_gold`, or
   raise `g_bracket_gold`'s gates to match hybrid's trade selectivity.
   Not a same-session cull — needs explicit trading decision.

2. **g_pullback_prem as parallel instance of PullbackContEngine.** Same
   class, different params (MOVE_MIN 30pt vs 20pt, 2× risk, tighter
   trail). Runs all 3 session slots (h07/h17/h23), not h07-only as
   config log claims. S17 audit: only 1 PullbackCont trade in 8 days,
   no PullbackPrem trades observed — both are effectively dark in live
   conditions despite shadow_mode=true. Low-priority: leave as-is, but
   if a future audit shows either never fires, candidate for cull.

3. **g_macro_crash parameter relaxation.** Original design: fires on
   ATR>8 / vol_ratio>2.5 / drift>6 (real macro events, documented
   Apr 2 2025 crash P&L: $5,304). Live config lowers to
   ATR>4 / vol_ratio>2.0 / drift>3 ("cover Asia spikes"). This turns
   the engine into a small-range breakout scalper — opposite strategy.
   S17 audit: 8 trades / 12.5% WR / -$4.48/trade. Recommend reverting
   to original macro-only thresholds, not culling the engine.

---

*Document generated 2026-04-24, Session 13 Stage 1, Claude.*
*Repo HEAD at audit: `850e8fa6`.*
