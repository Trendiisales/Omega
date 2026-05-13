# Session Handoff — 2026-05-12 (NZST), part E

Read this first next session. Builds on `SESSION_HANDOFF_2026-05-12d.md` (part D).
Covers the cost-gate rollout to the 4 part-C deferred residual engines (RSIReversal,
Breakout, MacroCrash, XauusdFvg).

## TL;DR
1. **Cost gate added to 4 deferred residual engines.** All four patches sandbox-syntax-
   clean. Uncommitted. The handoff's part-C "Item C deferred (5 engines)" is now 5/5
   complete (RSIExtremeTurn already had a `cost_gate` reference before this session;
   the other 4 are this session's work).
2. **All other S37 work from part D is unchanged.** S37-P1 (`176c746a`) still the only
   commit in this thread. S37-P2 (RiskMonitor wiring +54 in engine_init.hpp + +27 in
   UstecTrendFollow5mEngine.hpp) and S37-P3 (calibration +16 in RiskMonitor.hpp + +198
   in calibrate_risk_thresholds.cpp) remain applied-but-uncommitted from part D.
   Yesterday's 13 files from part B/C also still uncommitted.
3. **Audit found ~13-15 more fire-sites across 10 engines that may also need gating.**
   Operator decided to lock in the 4 here and continue next session with a clean
   context window. Full audit list below in §"Carry-over to next session".

## Patches applied this session (4 files, uncommitted)

Each patch follows the identical pattern: add `#include "OmegaCostGuard.hpp"` to the
top of the file, add an inline `ExecutionCostGuard::is_viable` check at the engine's
fire-decision point, with the same 1.5x cost-ratio used by the 9 engines patched
yesterday. Engines vary in TP-distance heuristic (some have a fixed TP, others use a
conservative R-multiple estimate).

### include/RSIReversalEngine.hpp
```
Include block: +1 line
  #include "OmegaCostGuard.hpp"     // 2026-05-12 cost gate -- see on_tick() entry block

Fire site (just before `pos.active = true` at the end of on_tick()):
  + 14 lines: gate block + cooldown on block
  TP estimate: sl_dist * 1.5  (engine has no fixed TP; RSI-based / BE / trail exits)
  Lot:         0.01  (hardcoded SHADOW-mode lot in this engine; see pos.size = 0.01 line)
  Symbol:      "XAUUSD"
  On block:    m_cooldown_until = now_s + COOLDOWN_S; return;
```

### include/BreakoutEngine.hpp
```
Include block: +1 line
  #include "OmegaCostGuard.hpp"     // 2026-05-12 cost gate -- see BREAKOUT_WATCH fire site

Fire site (after the chop-pause check, before `pos.active = true` at line ~1192):
  + 16 lines: gate block + state reset on block
  TP estimate: |edge.tp_price - entry_px|  (computed by compute_edge_and_execution above)
  Lot:         edge.size
  Symbol:      symbol  (CRTP member, const char* default "???")
  On block:    phase = Phase::FLAT; return {};
```

Note: BreakoutEngine already runs its own internal cost model
(`compute_edge_and_execution` uses `spread * cost_spread_mult` as a proxy). Layering
ExecutionCostGuard adds the unified per-symbol cost model on top. Both checks now run;
the unified one is intentionally stricter on FX/metals where commission applies.

### include/MacroCrashEngine.hpp
```
Include block: +1 line
  #include "OmegaCostGuard.hpp"     // 2026-05-12 cost gate -- see on_tick() entry block

Fire site (after `b_tp` is computed, before `// Init` block at line ~616):
  + 14 lines: gate block + cooldown on block
  TP estimate: atr * BRACKET_ATR_MULT  (the guaranteed bracket-floor TP distance)
  Lot:         lot  (computed from BASE_RISK_USD / (sl_pts * 100))
  Symbol:      "XAUUSD"
  On block:    m_cooldown_until = now_ms + COOLDOWN_MS; return;
```

Gates on the guaranteed-bracket TP (30% of position) because that's the worst-case
positive scenario. The 70% velocity-trail piece usually captures more; we don't credit
it in the gate.

### include/XauusdFvgEngine.hpp
```
Include block: +1 line
  #include "OmegaCostGuard.hpp"     // 2026-05-12 cost gate -- see open_position()

Fire site (in open_position(), after `size` computed, before `m_pos = LivePos{}` at line ~957):
  + 13 lines: gate block + plain return on block
  TP estimate: TP_ATR_MULT * m_atr14  (fixed TP per the v3 ACCEPTED config)
  Lot:         size  (computed from RISK_DOLLARS / (risk_per_unit * USD_PER_PRICE_PER_LOT))
  Symbol:      "XAUUSD"
  Spread:      bar.spread_mean  (clamped to 0.0 if non-positive)
  On block:    return;  (engine reschedules naturally on the next bar via on_bar_close)
```

This engine is bar-based, not tick-based at the entry decision, so the engine state
doesn't carry a cooldown timer. A plain return is fine; the next bar will re-evaluate
the FVG.

## Sandbox verification
```
g++ -std=c++17 -fsyntax-only -I include  on all four files
```
Result: only `#pragma once in main file` warning + the pre-existing `g_macroDetector`
(MacroCrashEngine line 236 / XauusdFvgEngine line 291) and `g_news_blackout`
(XauusdFvgEngine line 871) cross-TU global errors. None of these are at the modified
lines. My insertions compile cleanly.

The cross-TU global errors are expected when compiling these headers in isolation
outside main.cpp — they're explicitly noted in the OMEGA_BACKTEST `#ifndef` guards
already present in the file. Mac build via `cmake --build build -j` is the authoritative
verification.

## Carry-over to next session — engines still un-gated

Audit of `pos.active = true` patterns across `include/*Engine*.hpp` shows these
fire-sites still without an inline ExecutionCostGuard call:

```
include/GoldMidScalperEngine.hpp:615     1 fire site
include/BracketEngine.hpp:916            1 fire site
include/LatencyEdgeEngines.hpp:152       1 fire site
include/EMACrossEngine.hpp:488           1 fire site
include/HTFSwingEngines.hpp:353,737      2 fire sites
include/SweepableEngines.hpp:923,1343    2 fire sites
include/SweepableEnginesCRTP.hpp:971,1347   2 fire sites
include/CandleFlowEngine.hpp:778,922,1506   3 fire sites
include/CrossAssetEngines.hpp            multiple engines, pattern TBD
include/IndexFlowEngine.hpp              uses pos_ pattern; needs deeper read
```

Several of these are noted in engine_init.hpp as culled / inert:
- LatencyEdgeStack culled (S13)
- IndexHybridBracket (4 instances) culled (S11 P3b)
- GoldHybridBracket culled (S11 P3b)
- NoiseBandMomentum entry-dispatch removed (engine-cull audit)
- RSIExtremeTurn `enabled = false` (S52, 0/153 combos profitable)

Several are hard-pinned shadow-only and not currently firing live:
- GoldMidScalper (hard-pinned shadow on first deployment)
- All four IndexFlow instances (kShadowDefault, currently shadow)
- TrendPullback (4 instances, kShadowDefault)
- EMACross (kShadowDefault)
- CandleFlow (kShadowDefault, restored 2026-04-29 with audit-tightened gates)
- BracketEngine (hard-pinned shadow regardless of g_cfg.mode)

The gate applies regardless of shadow_mode — shadow PnL still needs to reflect real-cost
decisions. Next session should:
1. Read each of the ~10 remaining files and identify which are still firing (live or
   shadow) vs which are inert / stub.
2. Apply the gate to the live + shadow ones using the same pattern documented above.
3. Leave culled / stub engines alone (no point gating dead code).

## Repo state at session end

Source files modified THIS session (4 files, uncommitted):
```
M include/RSIReversalEngine.hpp     +15 lines (1 include + 14-line gate block)
M include/BreakoutEngine.hpp        +17 lines (1 include + 16-line gate block)
M include/MacroCrashEngine.hpp      +15 lines (1 include + 14-line gate block)
M include/XauusdFvgEngine.hpp       +14 lines (1 include + 13-line gate block)
                                    +61 lines total across 4 files
```

Still uncommitted from prior sessions (per part-D handoff):
```
M include/RiskMonitor.hpp                +16   S37-P3 calibration parser
M include/UstecTrendFollow5mEngine.hpp   +27   S37-P2 on_fire_hook member
M include/engine_init.hpp                +54   S37-P2 wiring block
M backtest/calibrate_risk_thresholds.cpp +198  S37-P3 calibrator extension
```

Plus yesterday's 13 files from part B/C cost-gate rollout to AUD/NZD/JPY+USTEC
Htf+PDHL+4 XAU TF engines + tick_gold `#if 0` wrap.

## Validation actions next session

Before commit:
```bash
cd ~/omega_repo
rm -f .git/index.lock      # sandbox quirk
# 1. Mac build (definitive verification)
cmake --build build -j

# 2. Inspect this session's diff
git diff --stat include/RSIReversalEngine.hpp \
                include/BreakoutEngine.hpp \
                include/MacroCrashEngine.hpp \
                include/XauusdFvgEngine.hpp
# Expected:
#   include/RSIReversalEngine.hpp   |  3 ++
#   include/BreakoutEngine.hpp      |  3 ++
#   include/MacroCrashEngine.hpp    |  3 ++
#   include/XauusdFvgEngine.hpp     |  3 ++
#   (plus the inline gate blocks at the fire sites)
# Actually ~61 lines insertions, 0 deletions across 4 files.

# 3. If Mac build clean: commit
git add include/RSIReversalEngine.hpp \
        include/BreakoutEngine.hpp \
        include/MacroCrashEngine.hpp \
        include/XauusdFvgEngine.hpp
git commit -m "Cost-gate rollout to RSIReversal / Breakout / MacroCrash / XauusdFvg
Completes the part-C 'Item C deferred' list (5 engines). RSIExtremeTurn
already had a cost_gate reference; this commit adds the remaining 4:
RSIReversal, Breakout, MacroCrash, XauusdFvg. Pattern matches the 9
engines gated in S37-P0a/P0b yesterday: include OmegaCostGuard.hpp +
inline ExecutionCostGuard::is_viable() at the fire-decision point with
cost_ratio_min=1.5. Per-engine TP-distance heuristic varies (see
SESSION_HANDOFF_2026-05-12e.md §Patches for details).

Sandbox syntax-check: clean on all four files. Only pre-existing
g_macroDetector / g_news_blackout cross-TU global errors remain (these
resolve in main.cpp's TU as documented in the existing OMEGA_BACKTEST
\#ifndef blocks).

No core code modified."
```

## Recommended commit ordering

Current ordering with this session's work folded in:

1. **Pending from part B/C** (still uncommitted as of part D handoff):
   - "S37-P0a: GBP/EUR/USTEC TF5m Plan A/B/entry sweeps + cost gate"
   - "S37-P0b: cost-gate rollout to AUD/NZD/JPY + USTEC Htf + PDHL + 4 XAU TF engines"
   - "S37-P0c: GBP/EUR/USTEC TF5m sweep harnesses + ledgers (research)"

2. **Already landed** (no action):
   - `176c746a` "S37-P1: USTEC TrendFollow5m promotion patch (shadow only)"

3. **Pending from part D** (still uncommitted):
   - "S37-P2/P3: RiskMonitor wiring + calibration for UstecTrendFollow5m"

4. **NEW — pending from this session (part E)**:
   - "Cost-gate rollout to RSIReversal / Breakout / MacroCrash / XauusdFvg"

5. **Future** (after the 2-month USTEC TF5m shadow window):
   - "S37-P4: USTEC TrendFollow5m live promotion"

6. **Carry-over for a future session** (the remaining ~10 engines):
   - "Cost-gate rollout to GoldMidScalper / BracketEngine / Sweepable... / CandleFlow / HTFSwing / etc."

## Soft conclusions worth carrying

1. **The cost-gate pattern is now a known mechanism on 13 engines.** The same 4-line
   change (include + gate + state-reset + return) applies to every engine that has an
   explicit fire-decision point with spread/TP-distance/lot in scope. Future engines
   should pick up the gate at the moment they're stamped, before they ever go to
   shadow.

2. **TP-distance estimate is the only judgment call.** Engines with a fixed TP at fire
   time (FVG, MacroCrash bracket leg, the FX London-open / Asian-open compression
   brackets) gate on the actual TP distance. Engines without a fixed TP (RSIReversal:
   RSI-based exits, BreakoutEngine: trail-managed) gate on a conservative R-multiple
   estimate. The 1.5R default (matching the `cost_ok` lambda in on_tick.hpp) is the
   right starting point; tighten only if backtest shows over-firing.

3. **The "no trade can happen unless costs are covered" property still has gaps.**
   The 4 patches here close the most-explicit gaps (part-C deferred residuals). The
   ~10 remaining fire-sites are mostly in engines that are culled, stub, or hard-shadow.
   The risk profile of leaving them unpatched is low (no live $ flow through them
   today), but per the user's instruction, every trade-firing engine should have the
   gate. Next session should finish the rollout.

4. **No core code was modified.** All changes are inside engine header files at
   identified fire sites. `OmegaCostGuard.hpp` itself was not modified (its existing
   `is_viable` is used as-is). `engine_init.hpp` was not modified. `on_tick.hpp`,
   `trade_lifecycle.hpp`, `order_exec.hpp` — none touched. The central chokepoints
   already in those files (the `cost_ok` lambda at on_tick.hpp:1065, the direct
   is_viable calls at on_tick.hpp:1354/1558 and trade_lifecycle.hpp:1914) are
   preserved as belt-and-braces; the new inline engine-level gates are the
   primary defence.

## Bookkeeping notes
- **`.git/index.lock` sandbox quirk** persists from part D. Run `rm -f .git/index.lock`
  before any git command in the sandbox.
- **Operator action still pending (Windows-side)**: S36-P5 VPS deploy via Windows
  PowerShell. Per part-D handoff, the VPS is still running pre-S33d code where
  `GoldMicroScalperEngine.hpp` was the real engine; that file is now a no-op stub on
  Mac HEAD. Until VPS deploys, only the VPS is running the old microscalper binary
  on account 8077780.

## Quick-reference files
| file | purpose | size |
|---|---|---|
| `include/RSIReversalEngine.hpp` | gated this session | ~25 KB |
| `include/BreakoutEngine.hpp` | gated this session | ~50+ KB (CRTP, multi-derived) |
| `include/MacroCrashEngine.hpp` | gated this session | ~40 KB |
| `include/XauusdFvgEngine.hpp` | gated this session | ~50+ KB |
| `include/OmegaCostGuard.hpp` | the gate implementation | 6 KB (unchanged) |
| `include/OmegaCostGate.hpp` | thin should_skip wrapper | 1 KB (unchanged) |
| `outputs/SESSION_HANDOFF_2026-05-12e.md` | this document | current |
