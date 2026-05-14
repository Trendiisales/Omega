# UTF5m Phase 1 Sweep Results — 2026-05-14 (NZST)

Session: part-T, follow-up to part-S handoff (`SESSION_HANDOFF_2026-05-14h.md`).
Sweep driver: `scripts/utf5m_sweep_p1.sh`.
Harness: `backtest/UstecTrendFollow5mBacktest.cpp` (S72 P0, commit `51487fa`).

## TL;DR

**S63 protection is empirically ADVERSE on `UstecTrendFollow5mEngine`.**
The mirror of the VWR USTEC.F pattern (parts K/L). Phase 2 fine-sweep is
SKIPPED — the Phase 1 signal is decisive. Phase 3 walk-forward on the
**baseline (S63-disabled, S37-on)** configuration is the recommended
next step before any `g_ustec_tf_5m.enabled = true` flip.

## Sweep configuration

- Tape: `/Users/jo/Tick/NSXUSD_merged.csv` (~150M ticks, ~2 years USTEC)
- Cells: 19 (2 reference + 7 loss-cut + 5 be-arm + 5 be-buffer)
- Mode: each axis cell `--mode tuned` + single S63 axis override; other
  two S63 fields stay at engine class defaults (0.08 / 0.05 / 0.02 as
  of part-L).
- Wall-clock: 344s (~18s/cell) — 30x faster than the pre-run 3-5 hr
  estimate. The harness instantiates the production engine and drives
  ticks directly, no re-implementation overhead.
- Output dir: `outputs/utf5m_p1_20260514_142448/` (gitignored)

## Self-consistency: PASSED

All four engine-default-level cells produced byte-identical output
(5533 trades, gross=-979.164808, avg=-0.176968, 119 TP, 0 SL, 381 PI,
2150 LC, 2883 BE). No override-apply bug.

```
ref tuned                : 5533,2929,52.94,-979.164808,...
loss-cut=0.08 (default)  : 5533,2929,52.94,-979.164808,...
be-arm=0.05   (default)  : 5533,2929,52.94,-979.164808,...
be-buffer=0.02 (default) : 5533,2929,52.94,-979.164808,...
```

## Reference cells

```
ref baseline (S63 OFF, S37 ON) : 1403 trades, gross=+928.879, avg=+0.662   NET POSITIVE
ref tuned    (S63 ON,  S37 ON) : 5533 trades, gross=-979.165, avg=-0.177   NET NEGATIVE
```

Magnitude of S63's adverse effect:

- Trade count: 1403 → 5533 (4x increase). S63 cuts trades early; the
  per-cell `cooldown_bars=1` then frees the cell to re-fire immediately
  into worse setups.
- The ~4130 additional trades S63 induces average -0.46/trade,
  dragging the engine from +929 to -979.
- Classic winner-amputation + churn pattern.

## Per-axis results

### Axis A: `--loss-cut` (LOSS_CUT_PCT, BE_ARM / BE_BUFFER at engine defaults)

```
level   trades  n_lc    n_be    gross_pnl     avg_pnl
0.00    4656    0       3370    -688.615441   -0.147899   <-- best in axis (LC disabled)
0.04    6425    3842    2372    -1241.956504  -0.193301
0.06    5872    2826    2691    -1147.160991  -0.195361
0.08    5533    2150    2883    -979.164808   -0.176968   (engine default)
0.10    5376    1705    3030    -970.546112   -0.180533
0.12    5215    1366    3098    -833.204217   -0.159771
0.16    5083    1000    3215    -862.623024   -0.169707
```

Even with LOSS_CUT disabled (BE_ARM/BUFFER still on), gross stays at
-688 — far below baseline +929. The BE_ARM ratchet at engine defaults
is doing significant damage on its own.

### Axis B: `--be-arm` (BE_ARM_PCT, LOSS_CUT / BE_BUFFER at engine defaults)

```
level   trades  n_lc    n_be    gross_pnl     avg_pnl
0.00    3325    2850    0       -150.328084   -0.045211   <-- best in axis (BE ratchet disabled)
0.03    7171    2047    4472    -1520.575145  -0.212045
0.05    5533    2150    2883    -979.164808   -0.176968   (engine default)
0.07    4810    2224    2130    -861.137337   -0.179031
0.10    4204    2269    1497    -604.065362   -0.143688
```

BE_ARM=0.00 is the best single-axis-off cell across the entire sweep
at -150, but still adverse vs baseline +929. With BE_ARM disabled,
LOSS_CUT alone still fires 2850 times and costs the engine ~$150
gross (raw pts × lot units).

### Axis C: `--be-buffer` (BE_BUFFER_PCT, LOSS_CUT / BE_ARM at engine defaults)

```
level   trades  n_lc    n_be    gross_pnl     avg_pnl
0.00    4745    1839    2428    -837.117925   -0.176421
0.01    5112    1975    2647    -965.852374   -0.188938
0.02    5533    2150    2883    -979.164808   -0.176968   (engine default)
0.04    6953    2682    3670    -1241.489929  -0.178555
0.06    8949    3447    4771    -1833.141681  -0.204843   <-- worst in entire sweep
```

Monotonic in the wrong direction: looser buffer = more BE_CUT fires =
more lost edge. The buffer is acting purely as a churn-multiplier with
no protective benefit.

## Decision

Per the embedded sweep decision rule:
- A cell PASSES if `trades > 0` AND `gross_pnl > 0`.
- An AXIS shows EDGE if its best cell beats baseline gross_pnl by 1.5x.

**Only the baseline cell passes (+929.88). No axis cell crosses
positive territory. No axis shows edge.**

Phase 2 fine-sweep is SKIPPED — the signal is decisive enough that
refining around any sub-optimal cell adds no information.

## Recommended next steps

1. **Phase 3 walk-forward on the baseline configuration**
   (`--mode baseline`, no axis overrides). Per the VWR S71 Phase 3
   lesson (handoff §"Important lessons" item 2), full-tape +929 could
   still be regime concentration — even bimodal regimes can average
   to a positive number that doesn't survive WF discipline.
   Required: 3+ of 4 windows positive with aggregate PF ≥ 1.20.
   The driver `scripts/vrev_wf_t1.py` should be adaptable with flag
   substitutions; same windowing logic applies.

2. **IF Phase 3 WF PASSES:** add per-instance S63 reverts to
   `engine_init.hpp` for `g_ustec_tf_5m` mirroring the part-K/L VWR
   precedent at `engine_init.hpp:597-672`:
   ```
   g_ustec_tf_5m.LOSS_CUT_PCT  = 0.0;
   g_ustec_tf_5m.BE_ARM_PCT    = 0.0;
   g_ustec_tf_5m.BE_BUFFER_PCT = 0.0;
   ```
   Then flip `g_ustec_tf_5m.enabled = true` at `engine_init.hpp:950`.
   Keep `shadow_mode = true` for 6 months of live confirmation per the
   engine's own caveat at `UstecTrendFollow5mEngine.hpp:23-25`.
   The S63 wiring stays in code (so future re-validation can re-test
   without rewiring); only the per-instance values are zeroed. This
   is exactly the part-K/L precedent for VWR US500.F / USTEC.F /
   EURUSD.

3. **IF Phase 3 WF FAILS:** closure memo mirroring the VWR S71 outcome.
   Engine stays disabled. Document which dimension failed (regime
   concentration? trade-count drop in low-vol windows? other?).

## Important context for the next session

- The "S63 is instrument-specific" lesson from the part-K handoff is
  now twice-confirmed: VWR USTEC.F (parts K/L revert) and now UTF5m.
  Both engines trade USTEC. Both have tight-tail / winner-amputation
  risk, not fat-tailed losses. S63 was calibrated for the latter
  regime (GER40 baseline justifies the cuts) and is structurally
  wrong for the former.
- The part-S handoff §"Recommended next-session focus" item 1
  ("Build `backtest/UstecTrendFollow5mBacktest.cpp`") is now done
  (S72 P0). Item 2 (S34-B-as-members decision) was resolved in this
  session via the phased approach: (b) for now, deferred (a) until
  needed. The S34-B promotion is unlikely to be needed now that the
  Phase 1 result says baseline is the right config.
- The Phase 0 full-tape baseline+tuned step I originally suggested
  (parsed against the 100K-tick smoke run) was implicitly absorbed
  by Phase 1's reference cells. No separate Phase 0 artifact exists.
- Per the VWR S71 P3 disk-pressure incident, future WF on this tape
  should design for sequential window-stream-then-delete rather than
  concurrent 4-window splits. The rewritten `vrev_wf_t1.py` v2 is
  the working pattern to inherit.

## Source data location

- Sweep summary: `outputs/utf5m_p1_20260514_142448/phase1_summary.csv`
- Cell reports: `outputs/utf5m_p1_20260514_142448/cells/*_report.csv`
- Cell trades: `outputs/utf5m_p1_20260514_142448/cells/*_trades.csv`
- Cell stderr: `outputs/utf5m_p1_20260514_142448/cells/*_stderr.log`

`outputs/` is gitignored; this memo is the only artifact that gets
committed.
