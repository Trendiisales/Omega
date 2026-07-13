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
