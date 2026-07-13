# Gold Structural Campaign Engine — Stage A CORE-only verdict (2026-07-13, session v)

Directive: SESSION_HANDOFF_2026-07-13u.md Part 2 (operator, post up-jump kill). Build order steps
1-3+7-8 executed: tick normalizer (1s slices), same-slot trailing-20-day time-of-day baselines,
first-pullback + compression detectors, CORE-only replay, matched-random control.

Harness: `backtest/gold_pullback_core_bt.cpp` (streaming C++, env-parametrized, FAMILY=1|2,
MODE=STRAT|RANDOM). Data (both integrity-CERTIFIED, real bid/ask fills so spread is paid, plus
pad grid 6/8/10/14bp RT => effective ~8/10/12/16bp):
- `/Users/jo/Tick/xau_2022bear_tick.csv` — 2022-06..09 bear, 17.8M ticks, med spread ~2.0bp
- `/Users/jo/Tick/xau_6mo_corrected.csv` — 2025-11..2026-04 bull, 46.5M ticks, med spread ~1.55bp

## FAMILY 1 — first-pullback continuation (spec top priority): CORE-FAIL
Spec-faithful: impulse 20-45bp in 30-300s, efficiency+activity vs same-slot baseline, retrace
20-45% on lower DOWN-LEG activity (recovery-leg excluded from the activity gate — early builds
that included it vetoed their own entries), no lower low, no micro-VWAP failure, local-high-break
entry + up-imbalance + VWAP reclaim + remaining>=REM, structural stop = trough-buffer.
- Every cell of 36+ swept (EFF x ACT x PB_ACT x timeout x REM, trail AND fixed-stop/measured-move
  TP exits) NEGATIVE at pad6, both files, both sides. Best ALL PF 0.39.
- Canonical cell: n=199 combined, avg net −8.7bp @pad6 => gross vs mid ≈ 0.0bp/trade.
- medMFE < medMAE nearly everywhere (e.g. 8.3 vs 9.3): post-break follow-through is nil-to-negative.
- Matched-random (100 seeds, same hour-of-day/side/stop/management): z=−0.76 (6mo), z=−0.17 (2022).
  The confirmed pullback break carries ZERO information over a random entry at the same hours.
- 3-5x SCALED variant (impulse 60-150bp / 5-30min, stops 14-19bp): still negative both files
  (ALL PF 0.19 / 0.64).

## FAMILY 2 — compression breakout: CORE-FAIL
Spec-faithful: RNG_WIN range in bottom-30% of same-slot 20-day range distribution, short-ATR
falling, no recent 40bp extension, spread stable, break+HOLD (never first touch; retest into
range up to 25% of height allowed), velocity+imbalance at entry, stop = range midpoint.
- Spec scale (5-20min ranges): all cells negative both files (best ALL PF 0.68@pad6; the
  compression premise itself caps height at 4-12bp, so remaining>=32bp is structurally unreachable
  — HMIN vs RNG_Q are in direct tension).
- SCALED variant (1h ranges, height>=25bp, stops ~28-34bp): 2022 negative (PF 0.60); 2025-26
  ALL −94bp (PF 0.89); ONLY positive cell = 2025-26 LONG n=26 +84bp PF1.21@pad6 — long-only in a
  raging gold bull, shorts −177bp, below every gate, and matched-random z=+1.68 (<2). Drift, not
  edge — the same signature as the killed H1 up-jump session cell (z=+1.39).

## ROOT CAUSE (the transferable finding)
Gold intraday structure at tick scale has MFE/MAE ≈ 1.0-1.5 after a "confirmed" trigger, with
structural risk units (stop distances) of 8-35bp. Effective retail RT cost (spread+slip+comm)
is 8-12bp => COST = 25-100% OF THE RISK UNIT. Zero-to-tiny gross edge minus a cost that large is
unconditionally negative; management (trail vs TP vs ride) only reshapes the distribution —
exactly the operator's own scoping of the up-jump kill ("layering cannot rescue a weak entry").
Contrast the crypto campaign parents that PASSED (UNI stop 135-216bp, trail 270bp): cost there
is ~5-8% of the risk unit. For gold to reach that geometry the structural event must be
100-250bp => H4/D1-scale structures — the scale the EXISTING live gold engines already trade.

## SCOPE (verify-kill rule)
KILLED: family 1 (first-pullback continuation) as gold CORE at 20-45bp AND 60-150bp scales;
family 2 (compression breakout) as gold CORE at 5-20min AND 1h scales. Both symmetric L/S, both
regimes, real-spread fills, matched-random controlled.
NOT killed / untested: family 3 (liquidity-sweep recovery — but it operates at 4-12bp,
BELOW family 1's scale, so the cost-floor math applies a fortiori; needs new basis before any
build), family 4 (VWAP reclaim — v1 filter-only per spec), family 5 (micro-range — ADD/MIMIC
only, moot without a CORE). The CORE/ADD/MIMIC campaign MECHANICS remain validated (crypto sim).
Live gold cells untouched.

## RECOMMENDATION
Stop pursuing tick-scale gold CORE detectors under the current cost model. If the gold campaign
engine is still wanted, re-anchor the CORE event at H4/D1 structure (100-250bp risk units, cost
<=10% of stop) and run the same staged program — or park until an execution path with materially
lower effective cost exists.

Repro (examples):
  clang++ -O2 -std=c++17 -o backtest/gold_pullback_core_bt backtest/gold_pullback_core_bt.cpp
  EFF=0.15 ACT=1.2 PB_ACT=1.0 PB_TIMEOUT=1200 REM=24 ./backtest/gold_pullback_core_bt /Users/jo/Tick/xau_6mo_corrected.csv
  FAMILY=2 ACT=1.5 RNG_WIN=600 HMIN=8 REM=12 HOLDSEC=30 ./backtest/gold_pullback_core_bt <file>
  MODE=RANDOM SPEC=<dump> PADS=6 SEEDS=100 STRAT_NET=<net> ./backtest/gold_pullback_core_bt <file>

---

# PART 2 — H4/D1 RE-ANCHOR (session w→x, 2026-07-13): family 1 D1-ANCHOR **PASSES STAGE A**

Operator decision post-Part-1: re-anchor CORE at H4/D1 structural events. Harness extended
(same detectors/mechanics, tick-mode output byte-identical — parity-diffed): M1-bar input
`ts,o,h,l,c,spr` auto-detected (fills = close∓spr/2, activity = bar-range bp vs same-slot
baseline), scale knobs UPQ_WIN/EFF_STRIDE/GAPCLOSE, and STOPMODE=1 = stop at the impulse
ANCHOR (full structural event = risk unit) instead of the pullback trough.

Data (ALL integrity-gate CERTIFIED): /Users/jo/Tick/ xau_m1_2024_2026.csv + xau_m1_2022bear.csv
(dukascopy-derived) + NEW histdata-built era files xau_h2013_m1.csv, xau_h2015_m1.csv,
xau_h2022full_m1.csv, xau_h2023_24_m1.csv (EST→UTC shifted, mid-bars + mean spread;
med spread 1.6-3.7bp by era). ~5.7yr / 5 regimes total. No era overlap in pooling.

## What FAILED at H4/D1 (kill-scope additions)
- Family 1 with TROUGH stops (spec default) at H4 (imp 100-250bp) and D1 (200-500bp): every
  ALL cell negative at pad6 across 18 cells both files; entry lands near trough → stops stay
  27-60bp → cost still 15-40% of risk; gross ≈ 0 vs mid. LOOSE-filter control (ACT off,
  EFF 0.2, n=124-137) still gross-negative ⇒ the failure is the entry information at that
  stop geometry, not filter strictness.
- Family 1 ANCHOR stops at H4 scale (100-250bp impulses): ALL negative both files (only
  2024-26 LONG-only positive = drift signature; 2022 longs PF 0.03).
- Family 2 compression at H4 (4-8h ranges, HMIN 60-100bp) and D1 (24h, HMIN 150): m24_26
  gross-negative every cell (PF 0.61-0.95 @pad0); D1 scale fires 0-3×/26mo (event-starved);
  2022 n≤7. Family 2 now dead at FOUR scales (5-20min, 1h, 4-8h, 24h). CORE-FAIL final.

## What PASSED: family 1, D1 scale + ANCHOR stop ("D1-ANCH" cell)
Cell (all env, frozen after discovery on m24_26 only — eras are true OOS):
  STOPMODE=1 IMP_LO=200 IMP_HI=500 DUR_LO=21600 DUR_HI=259200 EFF=0.35 ACT=1.3 SPR_MAX=1.5
  RET_LO=0.20 RET_HI=0.45 PB_ACT=0.85 PB_TIMEOUT=86400 UPR=0.55 REM=150 ARM=250
  TRAILSEC=86400 BREAKSEC=7200 STOPBUF=15 MAXHOLD=864000 VWTOL=30 UPQ_WIN=3600
  EFF_STRIDE=3600 GAPCLOSE=260000
Mechanic: 200-500bp impulse over 6-72h → 20-45% pullback → 2h-local-extreme break entry with
imbalance+VWAP reclaim → stop BELOW THE IMPULSE ANCHOR (med stop 213-251bp) → trail (24h
rolling extreme) armed at +250bp. Hold ~5-9 days. Symmetric L/S. RT cost 8-16bp = **3-8% of
the risk unit** — the passing-crypto-parent geometry, achieved for the first time in gold.

Per-era (frozen cell, ALL @pad6): 2013 n=12 +1122 PF2.03 | 2015 n=8 +1080 PF5.43 |
2022 n=11 −68 PF0.94 (worst era = breakeven) | 2023-24 n=8 +465 PF2.59 |
2024-26 n=21 +2301 PF2.59 (era z=+2.57). Direction adapts by regime (2013/15/22 short-heavy,
bulls long-heavy) — the symmetric detector does this, no regime gate involved.

POOLED (n=60): PF 2.18@pad6 / 2.14@8 / 2.10@10 / 2.06@12 / 2.02@14; win 63%; worst −352bp;
maxDD 861bp @pad8.
GATES: PF ladder ✓ (2.14 vs 1.35@8, 2.06 vs 1.10@12) | pooled matched-random (200 seeds/era,
same hod/side/stop/mgmt, per-seed era-sum) mean −4 sd 1524 → **z=+3.22** ✓ (every era's own
random ≈ 0 ⇒ not drift) | halves: 2013-22 PF2.06 / 2022-26 PF2.23 ✓ | L/S separate: LONG n=25
PF1.39@8 (marginal pass), SHORT n=35 PF3.04 ✓ | month dominance: top month 20% of net,
14/44 months negative ✓ | plateau: 13/13 one-knob neighbors pooled-positive (+1351..+6082
around +4900; IMP_LO=150 is BETTER; no ridge) ✓.

## Caveats / NOT yet cleared for wiring
1. **n=60 over 13yr** — sparse by construction (~9 trades/yr). Sizing must respect −352bp
   worst trade and 861bp trade-level DD.
2. **ADDITIVITY vs live gold engines UNPROVEN** (operator precondition). Live shadow ledger
   was reset 2026-07 (504B) → no forward record to overlap; must BT the incumbent gold
   engines (XauTf4h etc, registry recipes) over 2024-26 and check entry/PnL-stream overlap.
3. **Data ends 2026-04-24.** Histdata 202605/06 not yet published (fetch attempted, empty).
   Re-validate the frozen cell on May-Jul 2026 once fetchable before wiring.
4. LONG side PF1.39@8 is just above gate — monitor; SHORT carries most edge.
5. Histdata era spread is feed-indicative (2-4bp); pad ladder covers this (PF2.42@pad14).

Repro:
  clang++ -O2 -std=c++17 -o backtest/gold_pullback_core_bt backtest/gold_pullback_core_bt.cpp
  env <cell-envs-above> PADS=6,10 ./backtest/gold_pullback_core_bt /Users/jo/Tick/xau_h2013_m1.csv
  env <cell-envs> DUMP=/tmp/e.csv ... ; env <cell-envs> MODE=RANDOM SPEC=/tmp/e.csv SEEDS=200 SEEDNETS=/tmp/r.txt ...

---

# PART 3 — WIRE PRECONDITIONS CLOSED + ENGINE WIRED SHADOW (session z, 2026-07-13)

Operator order (13y handoff): "Run the additivity backtest and wire the gold engine if it passes."

## Precondition 1 — ADDITIVITY vs live gold book: PASS
Incumbent faithful dumps over the same 2024-26 window (2yr_XAUUSD_tick_fresh.h1/h4/m30,
prod configs, per-engine rider_dump harnesses): TF1h n=268, TF4h n=399, TFD1 n=119,
GVB n=38, TB30 n=770. D1-ANCH dump: harness re-run reproduces n=21 +2301bp PF2.59 exactly.
- Time overlap IS high (union same-dir 84% of D1-ANCH open time) — expected, the TF book is
  near-always-in-market. Overlap is not the additivity criterion; the PnL stream is:
- Daily MTM PnL corr (both books marked at daily M1 closes): vs ALL-incumbent combined
  r=−0.066; weekly r=−0.113; conditional on D1-ANCH-active days r=−0.133. Per book:
  TF1h −0.063, TF4h +0.096, TFD1 −0.177, GVB +0.054, TB30 +0.006.
- Incumbents' 10 worst MTM days: D1-ANCH negative on only 1/10 (and +613bp on one).
- Vol-normalized 50/50 combine: ret/DD 2.44 (incumbents alone) → 6.11 (combined).
- Same-dir stacking risk exists trade-by-trade (2026-03-06 L −282bp while book long):
  sizing note stands (worst −352bp), shadow 0.01.
Scripts: scratchpad additivity.py / additivity_mtm.py / additivity_cond.py (session z).

## Precondition 2 — May-Jul 2026 revalidation: RUN (data now published)
histdata XAUUSD T 202603-202606 fetched (get.php tk flow) → xau_h2026mar_jun_m1.csv
(115,395 bars 2026-03-01..06-26, integrity-gate CERTIFIED; also xau_h2026mayjun_m1.csv).
Frozen cell on Mar-Jun: n=4 +1239bp PF5.74 @pad6 (L1 +301, S3 +938).
- Cross-feed parity vs duka overlap: 03-16 S identical to the minute (+1021bp both feeds,
  same stop 245bp); 04-20 S same day/stop, gross 191 vs 197 (histdata fixed-EST DST hour
  offset); 03-30 L same event, feed-granularity variation (+307 vs +236).
- NEW OOS window May-Jun: ONE trade (2026-05-05 S −255bp gross = a normal stop-out inside
  the −352bp design worst), June quiet. n=1 = no statistical power either way; nothing
  contradicts the cell. Forward shadow ledger is the continuing revalidation.

## WIRED (S-2026-07-13z, SHADOW 0.01)
`include/GoldCampaignD1AnchEngine.hpp` — faithful M1-path port of the frozen cell
(tick→UTC-M1 internal aggregation == research-file semantics; detector on closed M1 bars;
anchor stop; 250bp-armed 24h rolling trail; MAX_HOLD 10d; ExecutionCostGuard on entry;
auto-retirement latch −1725bp = 2x worst pooled DD; persistence wire_cross; M1 warm-seed
phase1/signal_discovery/warmup_XAUUSD_M1.csv 32,877 bars May-25..Jun-26 2026; heartbeat).
REAL-ENGINE PARITY (backtest/gold_campaign_parity.cpp): 2024-26 20/20 closed trades match
the harness dump to the entry-second/side/±0.5bp (21st = open-at-EOF, harness end-flush);
2013 era 12 trades +1122bp@pad6 EXACT; 2022 era 11 trades −68bp EXACT.
Deviations from harness (stated): no GAPCLOSE data-gap flush (live market closure ≠ data
gap; MAX_HOLD covers), entry/exit fills at REAL bid/ask instead of c∓spr/2 (identical in
parity mode by construction), detection paused while position open (equivalent — BT anchors
during an open window are gated by ts ≥ last_exit anyway).
