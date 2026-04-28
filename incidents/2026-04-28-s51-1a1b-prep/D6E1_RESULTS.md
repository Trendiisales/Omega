# S51 1A.1.b D6+E1 — Results

**Date:** 2026-04-28 (NZST), commits shipped 2026-04-28T11:43:18Z (UTC)
**HEAD:** d1d09db62d4f4389c903e812ffab8d0b67d8a1d8
**Run:** `/Users/jo/omega_repo/sweep_D6E1_20260428_231956/`
**Wall time:** 1036.5s (17.3 min) on 154,265,439 ticks
**Ticks file:** `/Users/jo/Tick/duka_ticks/XAUUSD_2024-03_2026-04_combined.csv`

## Pre-delivery verification

All numbers in this memo are extracted from the CSVs in the run output directory above. Top-N rankings come from `sweep_summary.txt`. Distribution counts come from awk over the `.csv` files. No verbal assertions are unverified.

## Headline

| Engine | D5 best score | D6 best score | Status |
|---|---|---|---|
| HBG | 0.4032 (combo #343) | **0.4377 (combo #261)** | D6 grid moved optimum; +8.6% on score |
| EMACross | -0.04 best non-zero | -0.04 best non-zero | E1 filter clean (74/490 degenerate); engine has no edge |
| AsianRange | best 1-2 trades | best 1-3 trades | Same harness-side bug; **new finding: cross-instance non-determinism** |
| VWAPStretch | 0.5% WR over thousands | top-50 mostly <1% WR | Same structural pathology as D5 |

E1 verified clean: 74/490 degenerate combos correctly flagged, all degenerate combos have n_trades=0, FAST≥SLOW for all flagged.

## Engine 1: HBG — D6 grid rebase result

### Top combo

| Field | Value |
|---|---|
| combo_id | 261 |
| min_range | 6.0 |
| max_range | 25.398417 |
| sl_frac | 0.42 |
| tp_rr | 1.587401 |
| trail_frac | 0.25 |
| n_trades | 49 |
| win_rate | 63.27% |
| total_pnl | 0.5613 |
| stddev_q | 0.2825 |
| stability | 0.7797 |
| score | 0.4377 |

vs D5 top: 44 trades, 70.5% WR, total_pnl 0.47, score 0.4032. D6 trade count up 11%, total_pnl up 19%, WR down 7pts.

### Top-50 grid distribution

```
min_range  (D6 grid: 1.5..6.0):     43/50 at 6.0 (ceiling-clipped)
max_range  (D6 grid: 16,20,25,32,40,51,64): 27/50 at 16, 20/50 at 25.4, 3/50 at 20
sl_frac    (D6 grid: 0.21..0.84):   42/50 at 0.42 (centred — D6 worked)
tp_rr      (D6 grid: 1.0..4.0):     35/50 at 2.0 (centred — already good in D5)
trail_frac (D6 grid: 0.125..0.5):   38/50 at 0.25 (still floor-clustered)
```

### Findings

**Finding 1 — sl_frac D6 hit centre.** 42/50 of top-50 sit at sl_frac=0.42, the new base. D5 had 33/50 at sl_frac=0.5 (old base). Grid is now correctly centred on the optimum.

**Finding 2 — max_range D6 found a *floor* optimum, not a ceiling one.** Hypothesis going in: D5's 19/50 ceiling-clipping at 25 meant the optimum was above 25. Reality: when max_range range expanded to 16..64, **27/50 migrated to the new floor of 16**, with another 20/50 at 25.4. The upper half of the grid (32, 40, 51, 64) is empty in top-50. The actual optimum may sit *below* 16 — the D5 minimum of 12 was closer than the D6 grid centre of 32.

**Finding 3 — min_range still ceiling-clipped.** 43/50 at 6.0, the D6 ceiling. Was 33/50 at 6.0 in D5 also at ceiling. **The min_range grid has never been moved upward.** The optimum may sit above 6.

**Finding 4 — trail_frac is dead, not just floor-clipped.** 38/50 at trail_frac=0.25 looks like floor-clipping at first glance, but combos #308–314 produce *byte-identical* `total_pnl=0.4561, stddev_q=0.3096, score=0.3483` while differing only in trail_frac (0.125, 0.157, 0.198, 0.250, 0.315, 0.397, 0.500). **If different trail_frac values produce identical outcomes, the trail logic literally never engages in those combos.** Every position must be hitting TP or SL before the trail arms. The grid concentration at 0.25 is therefore not a real optimum but an artefact of trail-frac being irrelevant. This needs a separate diagnostic (HBG_T trail_armed counter) to confirm.

**Finding 5 — trade count still below statistical confidence threshold.** Top combo: 49 trades over 24 months = 2.0 trades/month. n=49 at 63% WR has 95% CI of ±13 percentage points. D5 said the same about 44 trades. D6 increased trade count modestly but not into the n≥100 range needed for confidence.

### Recommended next moves on HBG (require explicit authorisation)

- **D6.1 max_range**: rebase 32→16 (geometric grid 8..32) to hunt the floor-side optimum. One-line constant change.
- **D6.2 min_range**: rebase 3→6 (geometric grid 3..12) to expose above-current-base territory. One-line constant change.
- **HBG-DIAG**: add `trail_armed_count` and `trail_fired_count` to HBG_T as runtime counters; dump per combo at sweep end. Confirms whether trail_frac is dead (which it appears to be) and if so, the param can be removed from the sweep entirely, freeing a slot for a more useful param.

## Engine 2: EMACross — E1 verified clean, engine has no edge

### E1 verification

```
Total combos:                  490
Degenerate flagged (FAST≥SLOW): 74  (15.10%)
Degenerate with n_trades > 0:   0   (correct — guard fires)
Spot-check FAST/SLOW pairs:
  combo flagged: FAST=9, SLOW=8   (FAST > SLOW)
  combo flagged: FAST=9, SLOW=9   (FAST = SLOW)
  combo flagged: FAST=11, SLOW=8  (FAST > SLOW)
```

E1 filter is functioning as designed.

### Engine result

Top non-degenerate combo (#385, FAST=9 SLOW=15 RSI_LO=80 RSI_HI=25 SL_MULT=1.5): 0 trades. RSI_LO=80 and RSI_HI=25 is a degenerate band (RSI_LO > RSI_HI means no entry can ever fire). E1 only filters fast/slow degeneracy; **rsi_lo > rsi_hi is a second degeneracy pattern not currently filtered.**

Once trades fire in volume (combos with FAST<SLOW and RSI_LO<RSI_HI), every combo loses heavily. Worst combo: total_pnl=-47.48 (~$4,748 over 24mo at 0.01 lot, scales to ~$237k at 5.0 lot — a real-money disaster).

### Recommended next moves on EMACross (require explicit authorisation)

- **E1.1**: extend E1 filter to also flag rsi_lo ≥ rsi_hi as degenerate. Low-cost addition to the existing filter.
- **No structural fix recommended.** EMACross has no statistical edge in any quadrant of the grid. Recommend deprioritising EMACross and removing it from the sweep until a redesign is proposed. (Decision deferred.)

## Engine 3: AsianRange — bug 1 confirmed, bug 2 newly visible

### Bug 1: 100× trade frequency mismatch (D5 finding reproduced)

```
n_trades histogram across all 490 combos:
   0 trades: 56 combos
   1 trade:  52 combos
   2 trades: 367 combos
   3 trades: 15 combos
```

Maximum trade count across all 490 combos: 3. Live `AsianRangeEngine` documented at 382 trades over the same window. Mismatch is unchanged from D5 (no AsianRange code changed in D6/E1). Root cause still unknown; D7 instrumentation required.

### Bug 2: cross-instance non-determinism (NEW FINDING)

10 combos in the sweep have **byte-identical IEEE-754 template parameters** — combos 24, 73, 122, 171, 220, 269, 318, 367, 416, 465. All have `(BUFFER, MIN_RANGE, MAX_RANGE, SL_TICKS, TP_TICKS) = (0.5, 3.0, 50.0, 80, 200)`. Verified at the bit level: every multiplier is exactly `0x3FF0000000000000` (1.0 in IEEE 754).

These 10 combos are the same templated type instantiated 10 times. They run on the same tick stream in the same order. They should produce byte-identical results.

Actual results:

```
I=24,  pair=0:  pnl=-0.0160  (LOSS, 0/2 wins)
I=73,  pair=1:  pnl=+0.0120  (WIN,  1/2 wins)
I=122, pair=2:  pnl=-0.0160  (LOSS, 0/2 wins)
I=171, pair=3:  pnl=+0.0120  (WIN,  1/2 wins)
I=220, pair=4:  pnl=+0.0120  (WIN,  1/2 wins)
I=269, pair=5:  pnl=+0.0120  (WIN,  1/2 wins)
I=318, pair=6:  pnl=+0.0120  (WIN,  1/2 wins)
I=367, pair=7:  pnl=+0.0120  (WIN,  1/2 wins)
I=416, pair=8:  pnl=+0.0120  (WIN,  1/2 wins)
I=465, pair=9:  pnl=+0.0120  (WIN,  1/2 wins)
```

**Combos 24 and 122 (pair index 0 and 2) lose; the other 8 win.** Two combos diverge from the other eight despite identical params.

### Investigation status

`AsianRangeT` class itself contains no mutable static state — all `static` declarations are `static constexpr` compile-time constants. State is per-instance member variables. The class is well-formed for parallel use.

The bug source must be outside the engine class — likely candidates are the harness `ManagedPos` array, `SnapshotBuilder` shared state, or some interaction with the engine tuple's per-instance initialisation order. **Without instrumentation, root cause cannot be pinpointed.**

### Implication

This bug is potentially more important than Bug 1. If 10 instances of the same engine on the same data produce different P&L, **every other engine's sweep results may also be subtly contaminated** — we just can't see it because the other engines don't have a 10-way identical-params replication available to expose it.

The HBG sweep does have similar identical-multiplier replicates (also at I % 49 == 24). They appear to produce more consistent results in spot-check, but a full replication-determinism check across all 4 engines is now warranted.

### Recommended next moves on AsianRange (require explicit authorisation)

- **D7 (revised)**: AsianRange gate-rejection counters (original D7 spec) PLUS per-instance state dump for combos 24, 73, 122 at sweep end. The state dump (last_signal_s_, asian_hi_, asian_lo_, signal_count_, last_day_, long_fired_, short_fired_) at end of run will pinpoint which state field diverges.
- **D7.1 cross-engine determinism check**: print all 10 centre-combo results for HBG and VWAPStretch as well. Confirm whether non-determinism is AsianRange-specific or harness-wide.

## Engine 4: VWAPStretch — confirmed broken, structurally

D5 finding reproduced: top combo 189 has `total_pnl=-4.03, n_trades=1990, win_rate=35.6%`. 27/490 combos have WR>30%, all clustered at sl_ticks=80 (wide stop). All lose.

D8 (structural fix) authorisation carried forward. Not started.

## Cross-cutting findings

### Finding A: harness `write_csv` silent-failure pattern

`backtest/OmegaSweepHarness.cpp:1095` — `write_csv` checks `fopen` and prints `ERROR: could not open <path> for write` to stderr on failure, but main() continues regardless and emits `wrote sweep_xxx.csv (%zu combos)` to stdout unconditionally. The prior session's "1096s sweep reported complete but no CSV existed" symptom is consistent with this pattern (`ERROR` to stderr was lost, `wrote` to stdout was retained).

### Finding B: harness output path resolution is cwd-relative

`Args.outdir` defaults to `"sweep_results"` which is resolved relative to launch cwd. Recommend documenting in usage() and / or requiring absolute paths.

### Recommended cross-cutting commit (low-risk, high-value)

Add fail-loud behaviour to `write_csv`: propagate failure up to main, return non-zero exit code, emit a `FAIL: sweep_xxx.csv` line on stdout (not just stderr). One commit, ~10 lines, removes a class of "phantom complete" runs forever.

## State at memo write

- **Live VPS**: ed95e27c, untouched.
- **Repo HEAD**: d1d09db6.
- **Mac binary**: `~/omega_repo/build/OmegaSweepHarness`, 23:05 NZST 2026-04-28, contains D6+E1.
- **Authorised but not started**: D7, E2, D8 (per prior SESSION_HANDOFF.md).
- **Newly recommended (not authorised)**: D6.1, D6.2, E1.1, HBG-DIAG, D7.1, write_csv fail-loud commit.

## Decision required

The original sweep priority was HBG → DXY (skipped) → AsianRange → VWAPStretch → EMACross. After D5+D6+E1, the data suggests a different priority:

1. **D7+D7.1** first — the determinism bug must be resolved before any of the broken engines' results can be trusted.
2. **HBG-DIAG** — confirms whether trail_frac is dead. If yes, drop trail from sweep, free the slot.
3. **D6.1/D6.2** — only after D7+D7.1 confirm AsianRange harness path is sound.

D8 (VWAPStretch structural fix) and EMACross redesign are both substantial investments against engines that may not have a real edge. They should wait until D7 is resolved.
