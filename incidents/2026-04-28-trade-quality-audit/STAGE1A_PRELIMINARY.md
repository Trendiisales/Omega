# Stage 1A — Trade Quality Audit (Source-Level Findings)

**Session:** 2026-04-28 NZST
**Scope:** Diagnose bad trades from yesterday + today's session.
**Method:** Source-level audit at VPS commit `ed95e27c` (byte-identical to HEAD `33e4ffe3` for `GoldEngineStack.hpp`).
**Status:** PRELIMINARY — log evidence not yet pulled.

---

## Finding 1: Live engine roster is much larger than userMemory states

**userMemory says:** "Live engines: GoldFlow removed, HybridBracketGold + MacroCrash (disabled) + RSIReversal + MicroMomentum (disabled)"

**Source says (GoldEngineStack.hpp at VPS commit ed95e27c):** 19 engine classes registered, 18 of them live.

| # | Engine | Source line | Default state | Comment |
|--:|---|---:|---|---|
| 1 | SessionMomentumEngine | 285 | LIVE (default) | 07:15-10:30, 13:15-15:30 UTC only |
| 2 | MomentumContinuationEngine | 398 | LIVE (default) | "Shadow for 50 trades before enabling live -- start in SHADOW mode" (comment only, no flag set) |
| 3 | IntradaySeasonalityEngine | 491 | LIVE (default) | |
| 4 | DonchianBreakoutEngine | 643 | LIVE (default) | |
| 5 | NR3BreakoutEngine | 808 | LIVE (default) | |
| 6 | SpikeFadeEngine | 991 | LIVE (default) | |
| 7 | AsianRangeEngine | 1149 | LIVE (default) | |
| 8 | DynamicRangeEngine | 1292 | LIVE (default) | |
| 9 | NR3TickEngine | 1473 | LIVE (default) | |
| 10 | TwoBarReversalEngine | 1582 | LIVE (default) | |
| 11 | MeanReversionEngine | 1736 | LIVE (default) | |
| 12 | VWAPSnapbackEngine | 1896 | **LIVE (explicit)** | "Re-enabled: 1T sample too small for judgment" |
| 13 | LiquiditySweepProEngine | 1942 | LIVE (default) | |
| 14 | LiquiditySweepPressureEngine | 2024 | **DISABLED (explicit)** | "DISABLED: 51T 29%WR -$12" |
| 15 | LondonFixMomentumEngine | 2128 | LIVE (default) | |
| 16 | VWAPStretchReversionEngine | 2231 | LIVE (default) | |
| 17 | OpeningRangeBreakoutNYEngine | 2416 | LIVE (default) | |
| 18 | DXYDivergenceEngine | 2577 | LIVE (default) | "RE-ENABLED with real DX.F feed... Shadow for 30 trades to validate beta calibration" (comment only) |
| 19 | SessionOpenMomentumEngine | 2690 | LIVE (default) | |

**EngineBase default is `enabled_=true`** (line 263). Any engine without explicit `enabled_=false` in its constructor is firing.

**Plus** HBG bracket layer (GoldHybridBracketEngine.hpp), MacroCrash (disabled per engine_init.hpp L114), RSIReversal (enabled per engine_init.hpp L164).

**Net live on Gold:** ~19 distinct entry engines (18 stack + HBG bracket).

**Implication:** This is not a 4-engine system. Any analysis treating it as one (including the previous "live engines list" you've been reasoning about) is wrong. Bad-trade diagnosis cannot be done with a partial roster.

---

## Finding 2: Two of the bad trades you pasted come from "MomentumContinuation-class" engines

Trades pasted:
- 05:06:20 SHORT HybridBracketGold SL -$7.04
- 05:07:03 LONG VWAP_SNAPBACK SL -$5.28
- 05:30:18 SHORT HybridBracketGold TRAIL +$2.55
- 05:40:02 SHORT HybridBracketGold TRAIL +$0.04
- 07:34:35 SHORT SessionMomentum SL -$5.28

**05:06 vs 05:07 — opposing direction concurrent fire.** Within 43 seconds:
- HBG fires SHORT at 4649.51 (loses to 4656.49, -$6.98)
- VWAP_SNAPBACK fires LONG at 4653.41 (loses to 4648.41, -$5.00)

These two engines are independent classes with independent gates. **No source-level evidence yet of a cross-engine direction gate** that would prevent simultaneous opposite-direction entries on the same instrument. Need to read on_tick.hpp and tick_gold.hpp gate chains to confirm. If no such gate exists, this is structural — every paired contrarian engine (MeanRev/VWAPSnapback vs Momentum/SessionMom) can take the system on both sides of the same micro-move.

**07:34 SessionMomentum SHORT SL.** 07:34 is in the 07:15-10:30 SessionMomentum window (London open). Engine is firing at the right time. The SL fire isn't a gate failure — it's an entry-quality failure. SessionMomentum has IMPULSE_MIN=3.50 and VWAP_DEV_MIN=1.50 (per source L288-297). Need to see whether the entry actually had those conditions or whether something is calibrated wrong.

---

## Finding 3: Comments contradict source state

Several engines have "shadow first" or "validate before enabling" comments but are LIVE by default:

- **MomentumContinuation L398:** Comment says "Shadow for 50 trades before enabling live -- start in SHADOW mode". No `enabled_=false` in constructor. Engine is LIVE.
- **DXYDivergence L2577:** Comment says "RE-ENABLED with real DX.F feed... Shadow for 30 trades to validate beta calibration". No shadow flag. Engine is LIVE.

Either the shadow gating happens elsewhere (engine_init flag, runtime config) or these engines went live without the planned shadow validation. Need to check.

---

## What the logs need to confirm

Before any fix, the VPS logs need to show:

1. **Which of the 18 engines actually fired yesterday + today** (signal generation count per engine).
2. **Which fired entries vs which were gated off downstream** (engine emits signal → tick_gold gate → symbol gate → executed).
3. **For every executed trade: full gate trace** — what gates passed, which engine won, was there a competing signal from another engine the same tick.
4. **Were there opposing-direction signals in the same window** that should have been gated but weren't.

---

## Provisional VPS log commands

These are review-before-running. Confirm paths against actual VPS layout first.

### A. Daily file inventory (run first)

```powershell
Get-ChildItem C:\Omega\logs\trades\ -Filter "*2026-04-2*" | Select-Object Name, Length, LastWriteTime
Get-ChildItem C:\Omega\logs\gold\ -Filter "*2026-04-2*" | Select-Object Name, Length, LastWriteTime
Get-Item C:\Omega\logs\omega_service_stdout.log | Select-Object Name, Length, LastWriteTime
```

### B. Per-engine signal counts (after step A confirms filenames)

Need to know exact filename pattern from step A first. Then read with:

```powershell
$f = "C:\Omega\logs\trades\omega_trade_closes_2026-04-27.csv"
Get-Content $f | Select-Object -First 3   # confirm header structure
Import-Csv $f | Group-Object engine | Select-Object Name, Count | Sort-Object Count -Descending
```

### C. Stdout log size check (decides upload-vs-grep strategy)

```powershell
Get-Item C:\Omega\logs\omega_service_stdout.log | Format-List Name, Length, LastWriteTime
```

If under 50 MB, upload it whole. If larger, we'll do windowed reads keyed to the trade timestamps.

---

## Stage 1A next steps (not yet done)

- [ ] User runs Step A, pastes output
- [ ] Confirm filename patterns
- [ ] Pull trade ledgers for 2026-04-27 + 2026-04-28 UTC
- [ ] Tally signals per engine, executed trades per engine, P&L per engine
- [ ] Identify which engines are bleeding vs producing
- [ ] Read on_tick.hpp + tick_gold.hpp gate chains to find/confirm cross-engine gates
- [ ] Produce Stage 1A final findings doc with ranked culprits
- [ ] Decide whether fix is: disable underperforming engines, add cross-engine direction gate, recalibrate, or all three

---

## What this audit is NOT

- Not a fix proposal yet. No code changes recommended until logs reviewed.
- Not a full ecosystem audit (Stages 3-5 territory). Scoped to engine roster + recent trade quality only.
- Not a MacroCrash re-enable decision (Stage 1B). MacroCrash is currently disabled and that's not changing in this session.

---

## Provenance

Source files read (full content, via GitHub contents API at commit ed95e27c):
- include/GoldEngineStack.hpp (230,079 bytes) — 18 engines confirmed
- include/engine_init.hpp (98,243 bytes) — global flags reviewed
- include/tick_gold.hpp (130,685 bytes) — partial read, gate references found
- include/SweepableEngines.hpp — no relevant references
- include/engine_config.hpp — no relevant references
- include/on_tick.hpp — no relevant references found yet (not fully read)

Files NOT yet read (required for complete audit):
- include/GoldHybridBracketEngine.hpp (28,794 bytes) — HBG entry conditions
- include/MacroCrashEngine.hpp (52,868 bytes) — for Stage 1B
- include/on_tick.hpp (128,307 bytes) — main gate chain
- Full read of tick_gold.hpp gate logic
- Live ledger CSV files (require VPS access)
- omega_service_stdout.log (require VPS access)
