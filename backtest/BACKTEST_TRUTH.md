# BACKTEST_TRUTH.md — the one protocol that stops the tombstone oscillation

**Read this BEFORE backtesting, tombstoning, or re-validating ANY engine.**

## The disease this cures

Engines get tombstoned, then months later re-mined "in another guise", re-flagged as
promising, re-built, re-falsified — burning time and tokens on the same idea. Root cause:
**we have no single trusted number.** We have ~10 one-off **bar-replay** harnesses
(`fvg_core`, `fvg_regime`, `squeeze_xregime`, `sweep_flip`, `peachy_orb`, `vsr_bt`, …),
each optimistic in its own way, **plus** a live shadow ledger we underuse. So the same
idea oscillates between "PF 1.6 ship it" and "loses live, kill it" depending on the lens.

Two failure modes, ONE root (untrusted measurement):
1. **Wrongly-killed winner** — tombstoned on a *polluted* number (ledger artifact, 100× lot
   bug, phantom hold). e.g. the 2026-06-15 "6mo shadow-book BT" cull batch — GoldOrb from it
   was later un-tombstoned at PF2.38. Real edge lost until resurrected.
2. **Wrongly-resurfaced loser** — a bar-replay harness re-flags a dead thing as promising.
   e.g. FVG "trend-beta PF1.25" (2026-06-16) = the same artifact as the original FVG "PF1.65"
   that already lost live and got tombstoned. Pure token burn.

## The measured fact that drives the rules

**Bar-replay harnesses OVERSTATE by ~0.5–0.7 PF** vs the engine-faithful tick backtest.
Proven 2026-06-16: `fvg_core` said FVG PF1.65 → engine-faithful `fvg_engine_bt.cpp` (drives
the REAL engine class) said PF0.95 → which matched the live-losing tombstone. Cause:
within-bar trail look-ahead (peak set by bar-high, trail exit checked against the same bar's
low) + optimistic edge-fills the live tick engine can't reproduce. **A bar-replay PF is not a
deploy number. It is a discovery hint that must be discounted and then confirmed faithfully.**

## THE METRIC RULE — cull on PF/WR, NEVER on raw $ net (added 2026-06-18)

**A cull/keep verdict must be made on PF or WR, not on the dollar net.** Dollar net is
**contaminable** three proven ways — pnl double-multiply (engine ×usd_per_pt AND ledger ×100),
lot bug (1.0 vs 0.01 = 100×), phantom multi-day-hold entries. PF and WR are **ratios**: a uniform
scaling error cancels out, so they survive the contamination intact.

Proven 2026-06-18 (contamination blast-radius audit over the April shadow archive, 1327 trades):
the −$10k-class nets (MacroCrash, CandleFlow, …) were 100×-inflated **fakes**, yet every one of
those engines was a **genuine loser by PF** (0.01–0.85) — so the PF-based L108 cull batch was
**correct**. The ONLY wrongful kills were the two decided on **dollar net** — GoldOrb (−$784) and
Xau3BarMom30m (−$371) — both later resurrected (PF2.38 / PF1.29). **Had this rule been in force,
neither would have been wrongly killed.** The contamination's entire decision-impact was the
handful of $-net-based culls; everything judged on PF was safe.

Corollary: a single-symbol engine's PF is fully contamination-immune (uniform scaling). A
multi-symbol engine's PF can distort if only *some* symbols are mis-scaled — confirm per-symbol.

## THE PROTOCOL (in order — stop at the first that answers)

### 1. LEDGER FIRST (cheapest truth; usually ends it)
If the engine ever ran shadow/live, the answer is **already logged** in
`<log_root>/trades/omega_trade_closes.csv` (VPS `C:\Omega\logs\trades\`). Read it. Filter
artifacts (`tools/analytics/ledger_analytics.py --since`, phantom_hold>7d, oversized_metal_lot).
- If the clean ledger shows the verdict → DONE. Do not backtest.
- This is OMEGA.md rule #1 + ENGINE_BACKTEST_REGISTRY.md. We keep ignoring it.

### 2. TOMBSTONE-FIRST GATE (before re-mining anything dead)
Before re-validating a tombstoned idea:
- Read the tombstone (engine_init comment + memory file + TOMBSTONE_AUDIT.md).
- **Reproduce the kill on the faithful harness/ledger FIRST.** If you can't reproduce the
  loss that killed it, you DON'T trust your new harness — fix the harness, don't ship the idea.
- Only after reproducing the kill do you explore variants.

### 3. FAITHFUL ARBITER (the only thing that gates a deploy)
Deploy/enable decisions are gated by the **engine-faithful tick backtest** ONLY — a driver
that `#include`s and runs the REAL engine class tick-by-tick (template:
`faithful_engine_bt_TEMPLATE.cpp`; reference impl: `fvg_engine_bt.cpp`). NOT a bar-replay
re-implementation. Cross-regime (bear + bull) + both-WF-halves + cost-honest, per
HARNESS_FIDELITY_CHECKLIST.md.

### 4. BAR-REPLAY = DISCOVERY ONLY, ALWAYS HAIRCUT
Bar-replay sweeps (fvg_core et al.) are for cheap idea-discovery. Any number from them gets a
mandatory **−0.5 to −0.7 PF haircut** before it means anything. PF1.25 bar-replay − 0.6 = 0.65
= dead → do NOT spend tokens building it. Only a haircut number that still clears ~1.2+ earns
a faithful-arbiter run.

## One-line rule

**Ledger first → reproduce the kill → faithful arbiter gates deploy → bar-replay is a
discounted hint, never a verdict.**

See also: HARNESS_FIDELITY_CHECKLIST.md (baseline-reproduction discipline),
ENGINE_BACKTEST_REGISTRY.md (faithful recipe + known traps), TOMBSTONE_AUDIT.md
(which tombstones were killed on trustworthy vs polluted numbers).
