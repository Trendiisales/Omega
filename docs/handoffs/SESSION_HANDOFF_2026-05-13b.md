# Session Handoff — 2026-05-13 (NZST), part G
Direct follow-up to `SESSION_HANDOFF_2026-05-13a.md` (part F). Covers
the cost-gate rollout to the four part-F carry-over engines
(MinimalH4Breakout / MinimalH4US30Breakout / C1RetunedPortfolio /
GoldEngineStack). All edits are header-only; no core code touched.

## TL;DR
1. **All four part-F carry-over engines are now gated.** That closes
   out the audit-tooling lesson called out in part-F §"Soft conclusions
   #2" -- the broader sweep ran clean post-edit; only intentionally-
   ungated INERT engines (LatencyEdge, RSIExtremeTurn, SweepableEngines,
   SweepableEnginesCRTP) remain.
2. **GoldEngineStack's 18 sub-engines collapse to ONE gate.** The stack
   architecture funnels all sub-engine `Signal` emissions through a
   `best` selector and a single `pos_mgr_.open(gs, spread, ...)` call.
   Gating that one site at L4157 covers all 18 sub-engines.
3. **NOT committed yet.** The host-side `.git/index.lock` from the
   prior session is still present and the Cowork sandbox cannot remove
   it (Operation not permitted). Operator needs to run
   `rm -f ~/omega_repo/.git/index.lock` on the Mac and then run the
   commit script in §"Recommended commit ordering" below.
4. **Mac build verification still operator-side.** Sandbox can't run
   `cmake --build build -j` (no cmake, no winsock2.h). Sandbox syntax
   check via `g++ -std=c++17 -fsyntax-only` was clean across all four
   files.

## Files gated this session (4)

### include/MinimalH4Breakout.hpp
```
Include block: +1 line
  #include "OmegaCostGuard.hpp"  // see on_h4_bar() entry guard
Fire site: on_h4_bar() at the `pos_.active = true` assignment
(L461 pre-edit, L468 post-edit).
TP estimate: tp_pts = h4_atr * p.tp_mult
Lot:         size (post-clamp to [0.01, max_lot])
Symbol:      symbol.c_str()  (default "XAUUSD", configurable per instance)
On block:    return sig (empty signal; engine re-evaluates next H4 bar close).
```

### include/MinimalH4US30Breakout.hpp
```
Include block: +1 line
  #include "OmegaCostGuard.hpp"  // see on_tick() H4-close entry guard
Fire site: on_tick() H4-close branch at the `pos_.active = true`
assignment (L316 pre-edit, L325 post-edit). Wrapped in if/else so the
existing long_only-else block stays balanced.
TP estimate: tp_pts = atr_pre * p.tp_mult
Lot:         size (post-clamp to [0.01, max_lot]; DJ30.F is $5/pt)
Symbol:      symbol.c_str()  (default "DJ30.F")
On block:    sig stays empty (valid=false); skip pos_/sig assignments.
```

### include/C1RetunedPortfolio.hpp
```
Include block: +1 line
  #include "OmegaCostGuard.hpp"  // see Donchian / Bollinger cell entry guards
Two fire sites:
  L283 pre-edit -- C1DonchianH1LongCell::on_h1_bar (long-only Donchian
                   H1 breakout). TP = h1_atr14 * tp_atr.
  L410 pre-edit -- C1BollingerLongCell::on_bar (long-only mean-reversion;
                   NO fixed TP -- indicator exit). TP proxy = sl_pts * 1.5
                   (same pattern as CandleFlow / on_tick cost_ok lambda).
Lot:         lot_lp = max(0.01, size_lot)
Symbol:      symbol.c_str()  (default "XAUUSD" for both cells)
On block:    return 0  (no entry; cell re-evaluates next bar close).
```

### include/GoldEngineStack.hpp
```
Include block: +1 line
  #include "OmegaCostGuard.hpp"  // see GoldEngineStack::on_tick() pos_mgr_.open() gate
Fire site: GoldEngineStack::on_tick() at L4157 pre-edit -- the SINGLE
central `pos_mgr_.open(gs, spread, latency_ms, current_regime_name())`
call. All 18 sub-engines feed `Signal` into the `best` selector at
L4129-4152 and reduce to one open() call here, so one gate covers all.

TP estimate: tp_ticks_eff * GS_TICK_SIZE where
  - tp_ticks_eff = gs.tp_ticks if > 0, else max(4.0, gs.sl_ticks) * 2.0
    (matches GoldPositionManager::open() fallback at L3636)
  - GS_TICK_SIZE = 0.10 (XAUUSD tick; literal because pos_mgr_'s
    TICK_SIZE is private constexpr, cannot reference from outside)
Lot:         max(gs.size, 0.01)
Symbol:      "XAUUSD"  (stack is gold-only)
On block:    return GoldSignal{}  (empty signal; stack re-evaluates next tick).

Note: this gate sits BEFORE pos_mgr_.open(). pos_mgr_ itself has no
internal cost gate; this is the only cost-viability check between
best-signal selection and position opening. All 18 sub-engines now
have to clear the same per-symbol cost-ratio threshold (1.5x).
```

## Sub-engines now covered by the GoldEngineStack gate
Per part-F §"Carry-over to next session" #1, the 18 sub-engines:
  SessionMomentum, VWAPSnapback, LiquiditySweepPro, MeanReversion,
  DonchianBreakout, NR3Breakout, SpikeFade, AsianRange, DynamicRange,
  NR3Tick, TwoBarReversal, LondonFixMomentum, VWAPStretchReversion,
  ORBNewYork, DXYDivergence, SessionOpenMomentum, IntradaySeasonality,
  LiquiditySweepPressure.

Plus `MomentumContinuation` (L471 -- found in the audit, also feeds
the same `best` selector; gated implicitly via the central gate).

## Sandbox verification (this session)
```
g++ -std=c++17 -fsyntax-only -I include  on all 4 edited files
```
Result: only the `#pragma once in main file` sandbox-standalone
warning. No errors. The cross-TU `g_macroDetector` warnings noted in
part-F do not appear here because none of the four files reference
that global.

Mac build via `cmake --build build -j` was NOT runnable in the sandbox
(no cmake; project includes winsock2.h which the Linux sandbox doesn't
ship). Mac build remains the authoritative test on the operator side.

## Post-edit ungated audit
```bash
for f in include/*.hpp; do
  if grep -lE "pos[_]?\.active *= *true|pos[_]?\.open\(sig" "$f" >/dev/null 2>&1; then
    if ! grep -q "OmegaCostGuard" "$f"; then
      echo "UNGATED: $f"
    fi
  fi
done
```
Output:
```
UNGATED: include/LatencyEdgeEngines.hpp        # S13 culled
UNGATED: include/RSIExtremeTurnEngine.hpp      # S52 disabled (0/153 combos profitable)
UNGATED: include/SweepableEngines.hpp          # research-only sweep harness
UNGATED: include/SweepableEnginesCRTP.hpp      # research-only sweep harness
```
All four are INTENTIONALLY ungated per part-F §"Carry-over to next
session" #4. No production engine fires positions through them.

## Carry-over to next session

### 1. Commit + push (operator-side; sandbox cannot)
Lock file blocks sandbox commit. Operator action on Mac:
```bash
cd ~/omega_repo
rm -f .git/index.lock
git add include/MinimalH4Breakout.hpp \
        include/MinimalH4US30Breakout.hpp \
        include/C1RetunedPortfolio.hpp \
        include/GoldEngineStack.hpp \
        outputs/SESSION_HANDOFF_2026-05-13b.md
git commit -m "Cost-gate rollout: MinimalH4 + C1Retuned + GoldEngineStack"
git push origin main
```
Single commit covers all four engines because they share the same
pattern (OmegaCostGuard include + is_viable() gate at fire-site) and
were applied in the same session.

### 2. Mac build verification (operator-side)
```bash
cd ~/omega_repo
cmake --build build --target OmegaBacktest -j
```
**IMPORTANT:** Do NOT run `cmake --build build -j` on Mac. The main
`Omega` target requires Windows-only headers (`winsock2.h`,
`windows.h`) and CANNOT build on macOS. Prior handoffs (parts D / E /
F) recommended the bare `cmake --build build -j`, which always
failed on the Omega target -- earlier Claude sessions never noticed
because the cross-platform backtest targets succeed first and look
like everything passed. Always specify `--target OmegaBacktest`
(or another cross-platform target like `IndexBacktest`,
`TsmomCellBacktest`, `OmegaSweepHarness`) as the Mac canary.

OmegaBacktest is the best canary because it includes `engine_init.hpp`
which transitively includes every engine header. If OmegaBacktest
builds, every engine header is good. The main `Omega` (Windows live
binary) target is validated only on the VPS via `.\OMEGA.ps1 deploy`
step [6/12].

Sandbox `g++ -fsyntax-only` is a necessary-not-sufficient check;
the Mac OmegaBacktest build is the sufficient check on the Mac side.

### 3. VPS deploy S36-P5 still pending (operator-Windows action)
Unchanged from parts D / E / F. Until the VPS PowerShell deploy ships,
account 8077780 still runs the pre-S33d binary. Claude cannot perform
this step.

### 4. Two cost-gate-related observations to monitor
**a. GoldEngineStack 18-sub-engine gate is a SINGLE chokepoint.**
If a future session adds a NEW sub-engine to the stack that fires
positions via a different path (i.e. not through the `best` selector
+ pos_mgr_.open() pipeline), the gate won't cover it. Audit any new
GoldEngineStack sub-engine the same way as this session's audit:
trace from its `process(snap)` return to where its Signal is opened.

**b. GoldPositionManager::TICK_SIZE is private constexpr.**
The gate uses a local `GS_TICK_SIZE = 0.10` literal because TICK_SIZE
is not accessible outside the class. If TICK_SIZE ever changes (gold
broker fractional ticks, etc.), both copies must be updated. A future
cleanup could promote TICK_SIZE to namespace scope or add a public
getter, but per the operator preference "Never modify core code unless
instructed clearly" this was left alone.

### 5. Engines covered. Engines remaining
Cost-gate coverage as of this session:
```
LIVE / hard-shadow, GATED:
  EurusdLondonOpen, GbpusdLondonOpen, UsdjpyAsianOpen,
  AudusdSydneyOpen, NzdusdAsianOpen, UstecTrendFollow5m,
  UstecTrendFollowHtf, PDHLReversion,
  XauTrendFollow 2h / 4h / D1, XauThreeBar30m, XauusdFvg,
  RSIReversal, Breakout, MacroCrash,
  GoldMidScalper, BracketEngine (XAU + 12 FX/index instances),
  EMACross, H4Regime,
  CandleFlow (3 paths),
  IndexFlow (4 instances),
  VWAPReversion (4 instances),
  TrendPullback (2 LIVE instances),
  NoiseBandMomentum (gold_london),
  MinimalH4Breakout (gold, shadow)         <-- NEW THIS SESSION
  MinimalH4US30Breakout (DJ30.F, live)     <-- NEW THIS SESSION
  C1RetunedPortfolio (Donchian H1 + Bollinger, shadow)  <-- NEW THIS SESSION
  GoldEngineStack (18 sub-engines, hard-shadow)         <-- NEW THIS SESSION

INERT / CULLED -- intentionally not gated:
  LatencyEdgeEngines (S13 culled).
  SweepableEngines, SweepableEnginesCRTP (research-only sweep harness,
    not registered in live runtime).
  H1SwingEngine (g_h1_swing_gold.enabled = false).
  CrossAsset EsNq / Oil / BrentWti / FxCascade / CarryUnwind / ORB
    (all enabled=false).
  RSIExtremeTurn (S52 disabled, 0/153 combos profitable).
  NoiseBandMomentum non-gold-london instances (all enabled=false).
```

**Cost-gate coverage is now complete for every engine that can fire
a position in live or hard-shadow.** The "no trade can happen unless
costs are covered" property is true by construction at the engine
layer, on top of the central chokepoints already in `on_tick.hpp` and
`trade_lifecycle.hpp`.

## Validation actions next session
```bash
cd ~/omega_repo
rm -f .git/index.lock      # sandbox quirk

# 1. Confirm part-G commit landed (after operator commits)
git log --oneline -8

# 2. Mac build (cross-platform canary; main Omega target builds only on VPS)
cmake --build build --target OmegaBacktest -j

# 3. Re-run the broader audit (should be unchanged from this session)
for f in include/*.hpp; do
  if grep -lE "pos[_]?\.active *= *true|pos[_]?\.open\(sig" "$f" >/dev/null 2>&1; then
    if ! grep -q "OmegaCostGuard" "$f"; then echo "UNGATED: $f"; fi
  fi
done
# Expect: only LatencyEdgeEngines, RSIExtremeTurnEngine, SweepableEngines,
# SweepableEnginesCRTP -- all intentionally ungated.

# 4. Confirm GoldEngineStack gate is at L4157-area
grep -n "ExecutionCostGuard::is_viable" include/GoldEngineStack.hpp
```

## Recommended commit ordering as of part-G end
1. **Operator commits this session's work** (single commit covers all
   four files, listed in §"Carry-over #1" above). After commit:
   ```
   git log --oneline -6
   <new>   Cost-gate rollout: MinimalH4 + C1Retuned + GoldEngineStack
   bf5e7dd S37-P0c research: sweep harnesses + ledgers
   ebd97ca Cost-gate rollout: complete production-engine coverage
   7f2cc98 Cost-gate rollout to GoldMidScalper / Bracket / EMACross / ...
   ac707e3 S37-P2/P3: RiskMonitor wiring + calibration for UstecTrendFollow5m
   176c746 S37-P1: USTEC TrendFollow5m promotion patch (shadow only)
   ```
2. **Future** (after the 2-month USTEC TF5m shadow window):
   - "S37-P4: USTEC TrendFollow5m live promotion"
3. **Operator-Windows-side**:
   - S36-P5 VPS deploy via PowerShell.

## Build environment lesson (added 2026-05-13 part G)

**Mac cannot build the main `Omega` target.** It requires `winsock2.h`
and `windows.h`, which only exist on Windows. The earlier handoffs
recommending `cmake --build build -j` were technically wrong --
that command always failed on the Omega target on macOS. The reason
no one noticed for multiple sessions: the cross-platform backtest /
sweep / verify targets succeed first in parallel make output, and
the eye sees "Built target X" for those and assumes everything is
fine. The error gets printed in red but is buried by the surrounding
green "Built target X" lines.

**Correct workflow:**
- Mac: `cmake --build build --target OmegaBacktest -j` is the canary.
  OmegaBacktest pulls in `engine_init.hpp` which transitively includes
  every engine header. Clean build = all engine code is good.
- VPS: `.\OMEGA.ps1 deploy` step [6/12] is the authoritative build for
  the main Omega Windows live binary.

If a future session needs to validate something Mac-side and the
operator's incremental Mac build seems wedged, the fix is usually:

```bash
rm -rf build/CMakeFiles/Omega.dir   # nuke just the Omega target's stale cache
cmake -S . -B build                  # regenerate dependency graph
cmake --build build --target OmegaBacktest -j
```

This avoids the nuclear `rm -rf build` rebuild (~5 minutes on this
project) and only refreshes the broken target's dependency cache.

## Soft conclusions worth carrying

1. **Cost-gate rollout is complete.** No engine that fires a position
   in live or hard-shadow is now ungated. Future cost-gate work is
   maintenance only -- audit new sub-engines as they are added to
   GoldEngineStack or elsewhere.

2. **Audit pattern works.** The broader sweep introduced in part-F
   §"Validation actions" caught the four engines part-E missed and
   ran clean post-edit. Future sessions should keep using this audit
   as the standard check.

3. **The GoldEngineStack gate is architecturally clean.** Because the
   stack already routes all sub-engine Signals through a `best` selector
   and one open() call, one gate at the selector exit covers 18+
   sub-engines. Future stacks following this pattern get the same
   benefit. Conversely, stacks where each sub-engine has its own
   independent fire-site (e.g. C1RetunedPortfolio with 2 cells) need
   one gate per cell.

4. **The C1RetunedPortfolio Bollinger cell uses an indicator-exit (no
   fixed TP).** The cost gate uses `sl_pts * 1.5` as a TP proxy, same
   as the CandleFlow pattern from part-F. This is conservative -- the
   actual indicator exit may give bigger or smaller gross, but the
   gate at least guarantees the position can in principle reach 1.5x
   the SL distance before TP-equivalent and cover costs. If the
   indicator exit stops out earlier, the SL still triggers as before.

5. **No core code modified.** `OmegaCostGuard.hpp`, `engine_init.hpp`,
   `on_tick.hpp`, `trade_lifecycle.hpp`, `order_exec.hpp` untouched.
   All changes are inside engine headers. Central chokepoints in those
   core files remain as belt-and-braces alongside the new engine-level
   gates.

## Bookkeeping notes

- **`.git/index.lock` sandbox quirk**: persists across sessions when
  not cleaned up Mac-side. Sandbox cannot delete it (Operation not
  permitted). Operator must `rm -f` it on the Mac before any commit
  can land. This session's commit was deferred for that reason.

- **Operator action still pending (Windows-side)**: S36-P5 VPS deploy.
  Unchanged from parts D / E / F.

- **`pragma once in main file` warning** appears on every sandbox
  `g++ -fsyntax-only` check because headers are passed directly as
  inputs rather than transitively via main.cpp. This warning does
  NOT appear in the real Mac build TU and is sandbox-only noise.

## Quick-reference files
| file | purpose | size |
|---|---|---|
| `include/MinimalH4Breakout.hpp`     | gated this session                  | ~22 KB |
| `include/MinimalH4US30Breakout.hpp` | gated this session                  | ~25 KB |
| `include/C1RetunedPortfolio.hpp`    | gated this session (2 cells)        | ~27 KB |
| `include/GoldEngineStack.hpp`       | gated this session (1 site / 18 sub-engines) | ~190 KB |
| `include/OmegaCostGuard.hpp`        | the gate implementation (unchanged) | 6 KB |
| `outputs/SESSION_HANDOFF_2026-05-12e.md` | part E (reference) | reference |
| `outputs/SESSION_HANDOFF_2026-05-13a.md` | part F (reference) | reference |
| `outputs/SESSION_HANDOFF_2026-05-13b.md` | this document      | current |
