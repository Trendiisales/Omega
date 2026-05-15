# Session Handoff — 2026-05-15 part E (NZST)

Direct follow-up to part-D (same date). This session completed the
S63 LOSS_CUT/BE_RATCHET wiring for all active gold + index state-E
engines. Next session: FX + remaining cross-asset engines.

## TL;DR

1. **S97 landed** (`376d6c6`). S63 LOSS_CUT wired into 6 engines:
   BracketEngine, CandleFlowEngine, DonchianEngine, GoldMidScalperEngine,
   MacroCrashEngine, TsmomEngine. All class defaults — sweep harnesses
   queued for per-engine calibration.

2. **3 engines skipped** (no code change needed):
   - EMACrossEngine — ECE_CULLED=true since S18, functionally dead
   - GoldUltimateStrategy — enabled=false, engine inert
   - GoldMicroScalperEngine — no-op stub since S33d

3. **FX Opens are already State A** — all 5 (EURUSD, GBPUSD, AUDUSD,
   NZDUSD, USDJPY) have LOSS_CUT_PCT=0.03% wired and active with
   management-path checks. No work needed.

4. **Next session focus: S63 for remaining state-E engines** — primarily
   BreakoutEngine (14 FX + index + commodity instances via CRTP template),
   plus C1RetunedPortfolio, MinimalH4US30Breakout, MinimalH4Breakout,
   and HTFSwingEngines (IndexSwing lives here).

## Commits this session

| Commit | Message | Files |
|--------|---------|-------|
| `376d6c6` | S97: S63 LOSS_CUT wiring for 6 state-E engines | BracketEngine.hpp, CandleFlowEngine.hpp, DonchianEngine.hpp, GoldMidScalperEngine.hpp, MacroCrashEngine.hpp, TsmomEngine.hpp |

origin/main now at `376d6c6`.

## What was wired this session (detail)

| Engine | Flavor | LOSS_CUT | BE_ARM | BE_BUFFER | Rationale |
|--------|--------|----------|--------|-----------|-----------|
| BracketEngine | LC only | 0.10% | — | — | CRTP template (gold+FX+indices). Own trail+BE. 0.10% sits inside bracket range. |
| CandleFlowEngine | LC only | 0.05% | — | — | XAU. Own partial+trail+early-fail+stagnation. |
| DonchianEngine | Full trio | 0.05% | 0.04% | 0.01% | XAU breakout/trend. No existing BE. Per-cell (7 cells). be_armed_ reset on entry. |
| GoldMidScalperEngine | LC only | 0.05% | — | — | XAU scalper. Own BE lock (3.0pt trigger). |
| MacroCrashEngine | LC only | 0.15% | — | — | XAU macro-continuation. Wide ATR SL ($8-18). 0.15% = $5.55, sits below structural SL. Uses _close_all() for pyramids. |
| TsmomEngine | Full trio | 0.05% | 0.04% | 0.01% | XAU trend-following. No existing BE. Multi-position: be_armed per Position struct. Uses `continue` after erase in on_tick loop. |

## S63 state classification — complete inventory

### State A (S63 wired + active)
- VWAPReversionEngine: GER40 (class defaults)
- VWAPReversionEngine: SP, NQ, EURUSD (State B — deliberately 0.0, part-K/L evidence)
- IndexFlowEngine: SP, NQ, NAS, US30 (class-default route)
- IndexMacroCrashEngine: x4 (class-default route)
- XauusdFvgEngine (0.05%)
- PDHLReversionEngine (0.04%)
- RSIReversalEngine (0.05%, LC only)
- XauThreeBar30mEngine (0.05%)
- **NEW this session:** BracketEngine, CandleFlowEngine, DonchianEngine,
  GoldMidScalperEngine, MacroCrashEngine, TsmomEngine
- FX Opens x5 (EURUSD, GBPUSD, AUDUSD, NZDUSD, USDJPY) — all at 0.03%

### State E (no S63 — next session targets)
- **BreakoutEngine** — CRTP template, 14 shadow instances (5 FX + 5 index +
  SP/NQ/CL/BRENT). Biggest single-template impact. Same pattern as
  BracketEngine wiring.
- **C1RetunedPortfolio** — XAU retuned Donchian H1 long. Shadow.
- **MinimalH4US30Breakout** — DJ30.F. Shadow.
- **MinimalH4Breakout** — XAUUSD. Disabled (enabled=false). Skip or low priority.
- **HTFSwingEngines** — contains IndexSwingEngine (SP, NQ). Shadow. Uses
  fixed-pts SL design (documented in part-C bonus classification as
  "no S63, fixed-pts SL").

### Disabled / Dead (no S63 needed)
- EMACrossEngine (ECE_CULLED)
- GoldMicroScalperEngine (no-op stub)
- GoldUltimateStrategy (enabled=false)
- All 29 engines with enabled=false (see engine_init.hpp)

## Next-session focus (operator request: "FX engines checked same as gold and indices")

Priority order:

1. **BreakoutEngine S63 wiring** — single CRTP template covers 14 instances.
   Add LOSS_CUT_PCT member + management-path in the manage block. Need to
   read the engine's manage section first to identify insertion point and
   close function signature. FX instances: 0.03% band (matching FX Opens).
   Index instances: 0.07-0.08% band. Gold instance (if any): 0.05% band.
   Template default should be conservative middle — override per-instance
   in engine_init.hpp. Probably 0.05% default with per-instance overrides.

2. **C1RetunedPortfolio** — XAU H1 Donchian retuned. Shadow. Structurally
   similar to TsmomCell (multi-position trend). Full trio likely appropriate.

3. **MinimalH4US30Breakout** — DJ30.F index breakout. Shadow. Index band
   0.07-0.08%.

4. **HTFSwingEngines / IndexSwingEngine** — SP/NQ. Shadow. Fixed-pts SL
   design. May need special handling (documented as "no S63" in prior
   classification — revisit whether that's a deliberate design choice or
   just not-yet-done).

5. **MinimalH4Breakout** — XAUUSD but enabled=false. Low priority, skip
   unless time permits.

After this batch, the only remaining state-E engines will be the ~29
disabled ones — housekeeping-only priority per operator's framework.

## Uncommitted working tree

```
M  download_dukascopy.py          (multi-instrument support, unrelated to S63)
```

The download_dukascopy.py change was not committed with S97 (unrelated).
Commit separately if desired:
```bash
git add download_dukascopy.py
git commit -m "tools: multi-instrument Dukascopy downloader (PRICE_DIV_MAP)"
```

## Stash state

```
stash@{0}: On main: S67: g_minimal_h4_gold shadow_mode=false, parked until S63 rollout complete
```

Unchanged from part-K. Do not pop until gold promotion validation is run.

## Key memories to honour

- **Shadow = live for protection** — shadow engines get same S63 treatment
  as live. Only enabled=false is "inactive". (feedback_shadow_equals_live.md)
- **Fix = wiring + harness + sweep** — complete deliverable is hooks + C++
  harness + sweep + evidence-based values. This session did wiring with
  class defaults; harness+sweep is queued follow-up. (feedback_fix_means_active.md)
- **Never disable based on different strategy shape** — each strategy shape
  needs its own harness. (feedback_never_cross_regime_disable.md)
- **Read comment blocks before touching LOSS_CUT/BE values** — lesson from
  part-K. (project instructions / CLAUDE.md)

## Engine count summary

- **LIVE: 6** (all XAUUSD)
- **SHADOW: 54** (across all instruments, includes 2 effectively-dead stubs)
- **DISABLED: 29** (enabled=false)
- **S63 State A: ~25 engines** (wired and active)
- **S63 State E remaining: ~5 active engines** (next session targets)
- **S63 State E disabled: ~29** (housekeeping only)

## Pre-commit checklist (unchanged)

1. `cmake --build build --target OmegaBacktest -j` green on Mac
2. `git diff` — only intended changes
3. Comment block above any LOSS_CUT/BE setting has been read
4. S63 management-path additions have call-site activation in same commit
5. Build → diff-review → commit → push
