# SESSION 2026-05-02 (cont.) — USDJPY Asian Open: Phase 2 Fine Grid

## Headline

Phase 2 fine grid (queue item 1 from the trail-fix session) was run as a
3 × 3 × 5 sweep around the chosen winner: `MIN_RANGE ∈ {0.19, 0.20, 0.21} ×
SL_FRAC ∈ {0.75, 0.80, 0.85} × MFE_TRAIL_FRAC ∈ {0.10, 0.12, 0.15, 0.18, 0.20}`,
45 cells. Result: **the chosen winner stays as the chosen winner.** No
better OOS-stable cell was found; the apparent corner peak at
`SL=0.75, MFE=0.10` (best full-period PnL of $323.51) is overfit and
collapses on OOS to PF=1.09.

The grid also reveals more about the edge structure than the prior
single-axis sweep could: there is a 2-D plateau on `MR=0.20` (15/15 cells
profitable, PF 1.14–1.22), a cliff at `MR=0.19` (15/15 cells unprofitable,
PnL −$124 to −$221), and a partial degradation at `MR=0.21` (11/15
profitable but PF mostly ≤ 1.07). The strategy's edge is sharply tied to
that one MIN_RANGE level and is not a smooth peak in MR.

## Final config — unchanged

```cpp
// Override on top of the production engine defaults; only these three change.
static constexpr double MIN_RANGE       = 0.20;
static constexpr double SL_FRAC         = 0.80;
static constexpr double MFE_TRAIL_FRAC  = 0.15;
// Everything else stays at the production defaults documented in
// include/UsdjpyAsianOpenEngine.hpp.
```

Performance (from prior session, re-confirmed by smoke-test in this session):

| Period                  | Trades | WR     | avg_win | avg_loss | PF   | Net PnL  | DD      | Months prof. |
|-------------------------|-------:|-------:|--------:|---------:|-----:|---------:|--------:|--------------|
| TRAIN  2025-03..09      |    176 | 81.8 % |   $7.83 |  −$26.20 | 1.34 | +$288.94 | $118.01 | 5/7         |
| TEST   2025-10..2026-04 |    103 | 77.7 % |   $8.79 |  −$25.63 | 1.19 | +$113.38 | $141.04 | 3/7         |
| FULL   14 months        |    278 | 79.5 % |   $8.22 |  −$26.32 | 1.21 | +$317.10 | $202.51 | 8/14        |

## Why we keep the chosen winner

The fine grid found one cell with marginally higher full-period PnL
(`MR=0.20, SL=0.75, MFE=0.10` at $323.51) but its OOS profile is
materially worse than the chosen winner's:

| Config                         | Full PnL | Train PnL | Train PF | Test PnL | Test PF | Test DD |
|--------------------------------|---------:|----------:|---------:|---------:|--------:|--------:|
| **Chosen** SL=0.80, MFE=0.15   | $317.10  |  $288.94  |   1.34   | **$113.38** | **1.19** | **$141.04** |
| Mid plateau  SL=0.80, MFE=0.10 | $317.72  |  $304.42  |   1.36   |   $98.65 |   1.17  | $143.59 |
| Corner peak  SL=0.75, MFE=0.10 | $323.51  |  $351.61  |   1.45   |   $53.72 |   1.09  | $188.15 |

The corner peak trades **+$6 of full-period PnL for −$60 of OOS PnL** and
drops OOS PF from 1.19 to 1.09 — barely above the no-edge boundary. Its
train PF (1.45) is the highest in the table, which is itself a
classic overfit signature: the in-sample fit improves while the
out-of-sample fit degrades. Picking that cell would be picking
fragility.

The mid-plateau cell (`SL=0.80, MFE=0.10`) is statistically tied with
the chosen winner on OOS metrics (PF 1.17 vs 1.19, test PnL $98.65 vs
$113.38, test DD $143.59 vs $141.04). Either is defensible. The chosen
winner (`MFE=0.15`) is preferred because its full-period DD is tighter
($202.51 vs $215.90) without sacrificing OOS edge, and `MFE=0.15`
sits closer to the column's mean (the SL=0.80 column is flat at PnL
$309–$318 across all five MFE values, so `MFE=0.15` is the central
estimate of that flat segment).

## The PnL surface

### By (MR, SL), averaging across the 5 MFE values

```
MR\SL     SL=0.75      SL=0.80      SL=0.85    row mean
0.19    $-169.51     $-157.34     $-166.14    $-164.33
0.20    $ 277.03     $ 313.13     $ 235.41    $ 275.19
0.21    $  23.07     $  85.02     $  17.46    $  41.85
```

The MR=0.19 row is uniformly destructive across every SL value — it is
not a question of "the right SL fixes 0.19," it is that 0.19 admits
too much chop and SL choice cannot recover it. Trade count at MR=0.19
averages 348 vs 278 at MR=0.20 (25 % more trades, all of them
worse-quality). MR=0.21 admits *fewer* trades (avg 225) and the
strategy holds a marginal edge.

### MR=0.20 plateau, by (SL, MFE)

```
SL\MFE     0.10    0.12    0.15    0.18    0.20
0.75      $324    $285    $262    $257    $257
0.80      $318    $309    $317    $311    $311
0.85      $239    $234    $242    $236    $226
```

The `SL=0.80` row is the flattest (range $309–$318), the `SL=0.75` row
has the peak corner but degrades sharply with rising MFE (range
$257–$324), and the `SL=0.85` row is uniformly worse than the other two
(range $226–$242). The chosen winner sits in the middle of the flattest
row.

### Plateau summary stats

```
MR=0.20 plateau (15 cells):
  PnL range:  $225.68  ..  $323.51   (spread $97.83)
  PF range:    1.14    ..   1.22
  WR range:   78.0 %   ..  80.1 %
  All 15 cells profitable: True
  All 15 cells PF >= 1.14: True
```

This is a stable interior plateau in two dimensions, not an isolated
peak. The 1-D ridge from the prior session is real and extends in both
SL and MFE directions on top of MR=0.20.

## Phase 2 fine leaderboard (top 15)

```
rank label                          trades   WR  avg_w   avg_l    PF       PnL       DD  months
   1 MR0.20_SL0.75_MFE0.10             279 79.2   8.12  -25.36  1.22  $323.51  $199.73  8/14   ← OOS overfit
   2 MR0.20_SL0.80_MFE0.10             279 79.9   8.15  -26.77  1.21  $317.72  $215.90  8/14
   3 MR0.20_SL0.80_MFE0.15             278 79.5   8.22  -26.32  1.21  $317.10  $202.51  8/14   ← chosen winner
   4 MR0.20_SL0.80_MFE0.18             277 80.1   8.13  -27.17  1.21  $311.31  $203.99  8/14
   5 MR0.20_SL0.80_MFE0.20             277 79.4   8.39  -26.95  1.20  $310.58  $191.91  8/14
   6 MR0.20_SL0.80_MFE0.12             278 79.9   8.14  -26.74  1.21  $308.96  $211.67  8/14
   7 MR0.20_SL0.75_MFE0.12             278 78.8   8.14  -25.37  1.19  $285.34  $218.58  8/14
   8 MR0.20_SL0.75_MFE0.15             278 78.1   8.25  -25.05  1.17  $262.20  $214.32  8/14
   9 MR0.20_SL0.75_MFE0.20             277 78.0   8.41  -25.55  1.17  $257.21  $288.25  7/14
  10 MR0.20_SL0.75_MFE0.18             277 78.7   8.16  -25.80  1.17  $256.90  $249.46  7/14
  11 MR0.20_SL0.85_MFE0.15             278 79.5   8.24  -27.69  1.15  $241.79  $220.19  8/14
  12 MR0.20_SL0.85_MFE0.10             279 79.9   8.14  -28.16  1.15  $239.32  $233.24  8/14
  13 MR0.20_SL0.85_MFE0.18             277 80.1   8.15  -28.59  1.15  $236.32  $218.95  8/14
  14 MR0.20_SL0.85_MFE0.12             278 79.9   8.15  -28.14  1.15  $233.92  $229.28  8/14
  15 MR0.20_SL0.85_MFE0.20             277 79.4   8.36  -28.31  1.14  $225.68  $210.01  8/14
```

The MR=0.21 cells start at rank 16 (best PF 1.12, $163.60 PnL) and
degrade steadily; the MR=0.19 cells fill ranks 31–45 (all unprofitable,
PF 0.90–0.94).

## What this changes about the prior conclusion

Nothing material. The prior session's diagnosis was correct:

1. The trail-leak fix is the source of the edge.
2. `MR=0.20` is the right MIN_RANGE.
3. The chosen winner is OOS-best.

The fine grid adds three things:

- **Stronger evidence the edge is structural, not a single lucky cell.**
  15 plateau cells all profitable with PF in [1.14, 1.22]. A noise
  artefact would not produce that.
- **Confirms MR=0.19 is a cliff, not a soft slope.** The prior session
  noted "below MIN_RANGE=0.18 the strategy goes from 'many low-quality
  breakouts' to 'fewer high-quality breakouts'." The fine grid shows the
  transition is at 0.20 exactly, not somewhere in [0.18, 0.20]. MR=0.19
  is on the chop side.
- **Identifies the corner-peak overfit risk.** Without OOS-validating the
  apparent improvement we would have switched to `SL=0.75, MFE=0.10` and
  ended up with a worse OOS edge than the anchor.

## What this does NOT resolve

The OOS PF=1.19 / 3-of-7-profitable-test-months concern from the prior
session is unchanged. The fine grid validates that the chosen cell sits
in a stable region of parameter space; it does not validate that the
edge survives regime shifts. Walk-forward (queue item 2) is still the
next test that matters.

## Updated next-session queue — priority order

1. **Walk-forward instead of fixed train/test.**
   Promoted from priority 2 to priority 1. Item 1 (Phase 2 fine grid)
   is now done. 7-month chunks miss regime shifts. Run a rolling
   6-month-train / 1-month-test walk across the full 14-month horizon.
   If any individual test month tips deeply negative, the edge is
   fragile. This is the single most important next test.
2. **Same engine, USDJPY London hours (06–09 UTC).**
   Item 3 from prior queue. Cheapest test of "is this engine
   architecture sound or only working on Asian range data." Just
   override `SESSION_START_HOUR_UTC=6, SESSION_END_HOUR_UTC=9` on the
   chosen winner.
3. **Tokyo-fix exclusion (00:50–01:00 UTC).**
   Item 4 from prior queue. With MR=0.20 most fix-ranges should already
   be filtered out, but worth a one-cell confirmation.
4. **Restricted session windows {01–04, 02–04, 00–02} UTC.**
   Item 5 from prior queue. The first hour after Tokyo open is
   documented chop.
5. **Kelly sizing analysis.**
   Item 6 from prior queue. WR=79.5 %, avg_win=$8.22, avg_loss=$26.32,
   Kelly fraction ≈ 0.139 (half-Kelly = 7 %). Current LOT_MAX=0.20 is
   already aggressive vs the OOS PF=1.19 — do not raise it without
   2-week shadow gate.
6. **Mean-reversion variant.**
   Item 7 from prior queue. Now lower priority; breakout direction is
   the right one, the fine grid confirms that.

Item 7 in the prior queue (mean-reversion) stays at the bottom; the
divergence-attribution question raised in chat is also lower priority
than 1–2 above and is logged for later as an attribution pass on the
existing trade ledger (not a separate engine).

## Files modified / added this session

```
scripts/usdjpy_asian_trail_sweep.py     MODIFIED   added run_phase2_fine() and
                                                   PHASE2_FINE_GRID; wired
                                                   'phase2_fine' into main();
                                                   updated module-level CLI
                                                   docstring usage block.
                                                   Existing phase1/phase2/phase3/
                                                   oos/cell/leaderboard/all
                                                   subcommands UNCHANGED.

docs/SESSION_2026-05-02_USDJPY_ASIAN_OPEN_PHASE2_FINE.md    NEW    (this doc)

build/usdjpy_trail_sweep_results.csv    APPENDED  45 fine-grid rows + 4 OOS
                                                   rows. Now 88 rows total
                                                   (39 prior + 45 fine grid +
                                                    4 OOS validation rows).

backtest/usdjpy_bt/UsdjpyAsianOpenEngine.hpp        regenerated each cell;
                                                    final state resynced to the
                                                    chosen winner (MR=0.20,
                                                    SL=0.80, MFE=0.15).
build/usdjpy_asian_bt                              recompiled to chosen-winner
                                                    state.

include/UsdjpyAsianOpenEngine.hpp        UNCHANGED  md5 d514fce983cf10914c77d001311bd4be
                                                    confirmed identical to
                                                    session start. Production
                                                    header is OFF-LIMITS to the
                                                    sweep tooling per project
                                                    policy.
```

## Re-running this session's grid

```bash
cd ~/omega_repo
git checkout feature/usdjpy-asian-open

# Tick data (host path differs per machine):
export USDJPY_TICKS=$HOME/Tick/USDJPY

# Trail-axis sweep is resumable. Cells already in the CSV are skipped.
python3 scripts/usdjpy_asian_trail_sweep.py phase2_fine

# OOS the chosen-winner alternates manually (the auto-oos picks
# composites/sl rows by label prefix, which the fine grid does not use):
python3 scripts/usdjpy_asian_trail_sweep.py cell my_train \
    MIN_RANGE=0.20 SL_FRAC=0.80 MFE_TRAIL_FRAC=0.15 \
    --from 2025-03 --to 2025-09
python3 scripts/usdjpy_asian_trail_sweep.py cell my_test \
    MIN_RANGE=0.20 SL_FRAC=0.80 MFE_TRAIL_FRAC=0.15 \
    --from 2025-10 --to 2026-04

# Sorted leaderboard:
python3 scripts/usdjpy_asian_trail_sweep.py leaderboard
```

## Reminders

* GitHub PAT in `CLAUDE.md` (`ghp_9M2I…24dJPV4`) — please rotate before
  next session. (Same reminder as prior doc; not yet acted on.)
* Phase 2 fine grid took ~10 min wall-clock at ~12.9 s/cell, faster than
  the prior session's 33 s/cell estimate. Future sweeps can plan for
  this rate.
* `feature/usdjpy-asian-open` still branched off `omega-terminal`.
  Prior session's commits are local; this session's modifications
  (sweep script + new doc) are also local and uncommitted. Push when
  ready.
* Production engine in `include/UsdjpyAsianOpenEngine.hpp` is unchanged.
  The chosen winner config from the prior session is preserved.
  Promotion to live still requires a 2-week shadow paper run per the
  engine header's safety gate.
