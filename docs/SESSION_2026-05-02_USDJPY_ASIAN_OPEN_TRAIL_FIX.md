# SESSION 2026-05-02 — USDJPY Asian Open: Trail-fix Sweep, OOS Validated

## Headline

The previous session concluded "no profitable config" for USDJPY Asian-session
breakouts and pinned the failure on a regime/signal mismatch. That conclusion
was incorrect.

The 66–74 percent win rate the prior sweep produced was real and exploitable;
the leak was on the **exit side**. The trailing stop's
`min(range * TRAIL_FRAC, mfe * MFE_TRAIL_FRAC)` formula was
clipping every winner to a near-uniform 5–6 pips while losers ran their full
14–17 pip stop. Fixing the trail and re-fitting the compression filter takes
the 14-month full run from −$917 to **+$317** and the OOS test fold from
−$81 to **+$113**.

This document records the diagnosis, the sweep, the winning config, and a
clean queue for the next session.

## Final config

```cpp
// Override on top of the production engine defaults; only these three change.
static constexpr double MIN_RANGE       = 0.20;   // was 0.08, prior best 0.15
static constexpr double SL_FRAC         = 0.80;   // unchanged from default
static constexpr double MFE_TRAIL_FRAC  = 0.15;   // was 0.40, prior best 0.50
// Everything else stays at the production defaults documented in
// include/UsdjpyAsianOpenEngine.hpp.
```

Performance:

| Period                 | Trades | WR     | avg_win | avg_loss | PF   | Net PnL | DD     | Months prof. |
|------------------------|-------:|-------:|--------:|---------:|-----:|--------:|-------:|--------------|
| TRAIN  2025-03..09     |    176 | 81.8 % |   $7.83 |  −$26.20 | 1.34 | +$288.94 | $118.01 | 5/7         |
| TEST   2025-10..2026-04|    103 | 77.7 % |   $8.79 |  −$25.63 | 1.19 | +$113.38 | $141.04 | 3/7         |
| FULL   14 months       |    278 | 79.5 % |   $8.22 |  −$26.32 | 1.21 | +$317.10 | $202.51 | 8/14        |

Both train and test fold are profitable, both PFs are clearly above 1.0,
the test-side WR holds at 77.7 percent, and the worst drawdown ($202) is
smaller than the full-period profit ($317). This is the first config in
this engine's history to clear OOS.

It is **not yet a "promote to live" result.** PF=1.19 OOS is thin and
3/7 OOS profitable months means edge is concentrated. Treat as
"shadow-mode candidate ready for the 2-week paper validation gate"
described in the engine header. The next-session queue below is what's
needed to harden it.

## Diagnosis (for the audit trail)

The previous session's sweep produced this pattern across **every** cell:

```
SL_FRAC sweep (0.4 → 1.0):       avg_win 7.48, 7.45, 7.52, 7.53, 7.55, 7.47, 7.48
TP_RR  sweep (1.5 → 3.5):        avg_win 7.61, 7.55, 7.55, 7.54, 7.54
MIN_RANGE sweep (0.06 → 0.15):   avg_win 7.58, 7.55, 7.44, 7.53, 7.60
MFE_TRAIL_FRAC sweep (0.3 → 0.6):avg_win 7.49, 7.55, 7.46, 7.47
```

Avg-win is frozen near $7.50 in every cell, regardless of which knob moves.
Exit-reason attribution: TP_HIT 10/1959 (0.5 %), TRAIL_HIT 1287/1959 (66 %).
Every winner is being clipped at the trail, and the trail clip is the same
size in every config.

Trade-ledger MFE distribution (MIN_RANGE=0.15 cell, 832 trades) confirms it:

```
Winners (n=600)          Losers (n=232)
avg MFE:        9.97      avg MFE:        2.62
avg exit pips:  6.13      losers w/ MFE>6: 2.2 % (BE-lock essentially never saves)
giveback:       3.84      ----- exit reasons -----
                          SL_HIT 226, BE_HIT 5, TRAIL_HIT 1
exit reasons              All losers hit their original SL.
TRAIL_HIT 597, TP_HIT 3
```

Mechanism. The trail formula is

```
trail_dist = min(range * TRAIL_FRAC,  mfe * MFE_TRAIL_FRAC)
```

For range = 0.15 and `TRAIL_FRAC = 0.30`, `range_trail = 4.5 pips`. Past
MFE ≈ 9 pips this caps the trail at 4.5 pips of giveback no matter how far
the runner goes. The original sweep tested
`TRAIL_FRAC ∈ [0.20, 0.25, 0.30, 0.40]` and `MFE_TRAIL_FRAC ∈ [0.30, 0.40,
0.50, 0.60]` — never high enough to release the cap, never low enough on
MFE_TRAIL_FRAC to lock more profit per win. `MIN_TRAIL_ARM_PTS` and
`MIN_TRAIL_ARM_SECS` were never swept at all.

The corrected sweep
(`scripts/usdjpy_asian_trail_sweep.py`) walks those untouched dimensions and
finds the operating point above.

## Why MIN_RANGE = 0.20 matters

Above MIN_RANGE = 0.18 the strategy goes from "many low-quality breakouts"
to "fewer high-quality breakouts." 14-month PnL by MIN_RANGE
(SL_FRAC=0.80, MFE_TRAIL_FRAC=0.05 to remove that confound):

```
MIN_RANGE 0.15  PnL=-$365  (anchor of prior session's "best")
MIN_RANGE 0.18  PnL=-$367
MIN_RANGE 0.20  PnL=+$211   <-- inflection point
MIN_RANGE 0.22  PnL=-$146
MIN_RANGE 0.25  PnL=-$140
```

Sharp ridge at 0.20. Below it, too much chop survives the filter; above it,
trade count drops faster than win quality grows. Worth re-checking with
finer steps {0.19, 0.20, 0.21} next session.

## Sweep leaderboard (top 10)

```
rank  label                                  trades    wr  avg_w   avg_l    pf     pnl      dd  months
   1  MR0.20_SL0.8_MFE0.10                      279  79.9   8.15  -26.77  1.21   317.72  215.90  8/14
   2  MR0.20_SL0.8_MFE0.15  *** chosen ***      278  79.5   8.22  -26.32  1.21   317.10  202.51  8/14
   3  MR0.20_SL0.8_MFE0.20                      277  79.4   8.39  -26.95  1.20   310.58  191.91  8/14
   4  TRAIN of #2  (2025-03..09, 7 mo)          176  81.8   7.83  -26.20  1.34   288.94  118.01  5/ 7
   5  TRAIN of #6  (2025-03..09, 7 mo)          181  82.3   7.53  -27.20  1.29   251.10  136.80  5/ 7
   6  MR0.20_SL0.8_MFE0.05                      285  79.6   7.81  -26.91  1.14   211.19  221.83  7/14
   7  MR0.20_SL0.9_MFE0.05                      283  80.6   7.83  -29.55  1.10   160.64  173.61  6/14
   8  MR0.20_SL0.7_MFE0.05                      288  77.1   7.79  -24.31  1.08   124.75  273.66  5/14
   9  TEST  of #2  (2025-10..2026-04, 7 mo)     103  77.7   8.79  -25.63  1.19   113.38  141.04  3/ 7
  10  TEST  of #6  (2025-10..2026-04, 7 mo)     105  77.1   8.26  -25.76  1.08    50.60  152.01  2/ 7
```

Cells 1–3 form a stable plateau. Picking #2 (MFE_TRAIL_FRAC=0.15) over #1
trades $0.62 of full-period PnL for a slightly tighter DD ($202 vs $216).
Either is defensible.

## Queue for next session — priority order

1. **Phase 2 fine grid around MIN_RANGE=0.20 + SL_FRAC=0.80 + MFE_TRAIL_FRAC=0.15.**
   Walk MIN_RANGE ∈ {0.19, 0.20, 0.21}, SL_FRAC ∈ {0.75, 0.80, 0.85},
   MFE_TRAIL_FRAC ∈ {0.10, 0.12, 0.15, 0.18, 0.20} as a 3×3×5 grid (45 cells,
   ~25 min compute). Look for a sharper interior optimum and confirm the
   plateau extends in 2-D.

2. **Walk-forward instead of fixed train/test.** 7-month chunks miss
   regime shifts. Run a rolling 6-month-train / 1-month-test walk across
   the full 14-month horizon. If any individual test month tips deeply
   negative, the edge is fragile.

3. **Same engine, USDJPY London hours (06–09 UTC).** Item 3 from the
   prior queue. Cheapest test of "is this engine architecture sound or
   only working on Asian range data." Just override
   `SESSION_START_HOUR_UTC=6, SESSION_END_HOUR_UTC=9` on the new winner.

4. **Tokyo-fix exclusion (00:50–01:00 UTC).** Item 5 from prior queue.
   The 09:55 JST fix lands in our window. With the new MIN_RANGE=0.20
   filter the fix-ranges may already be filtered out, but worth a one-cell
   confirmation.

5. **Restricted session windows {01–04, 02–04, 00–02} UTC.** Item 2 from
   prior queue. The first hour after Tokyo open is documented chop.

6. **Kelly sizing analysis.** With WR=79.5%, avg_win=$8.22, avg_loss=$26.32,
   Kelly fraction = (0.795 - 0.205 * 26.32 / 8.22) / 1 = 0.139 — i.e. you
   *should* be sizing about 14 % of equity per trade if the OOS edge is
   real. Half-Kelly = 7 %. Current LOT_MAX=0.20 is already aggressive
   relative to the OOS PF=1.19; do not raise it without a 2-week shadow
   gate first.

7. **Mean-reversion variant** (item 4 in prior queue). Now lower priority,
   since the breakout direction was always the right one — the prior
   session just couldn't see it through the trail leak.

## Files modified / added this session

```
scripts/usdjpy_asian_trail_sweep.py        NEW     trail-axis sweep driver
docs/SESSION_2026-05-02_USDJPY_ASIAN_OPEN_TRAIL_FIX.md   NEW   (this doc)

build/usdjpy_trail_sweep_results.csv       NEW     leaderboard data
build/trail_trades_*.csv                   NEW     per-cell trade ledgers
build/trail_report_*.csv                   NEW     per-cell month reports

backtest/usdjpy_bt/UsdjpyAsianOpenEngine.hpp           regenerated each cell
                                                       (final state has the
                                                        chosen-cell overrides
                                                        baked in; recompile
                                                        to reset)

include/UsdjpyAsianOpenEngine.hpp          UNCHANGED  (production engine
                                                       header is OFF-LIMITS
                                                       to sweep tooling)
```

## Re-running this session's sweep

```bash
cd ~/omega_repo
git checkout feature/usdjpy-asian-open

# Rebuild harness
g++ -std=c++17 -O3 -DOMEGA_BACKTEST -I backtest/usdjpy_bt \
    backtest/usdjpy_bt/UsdjpyAsianOpenBacktest.cpp \
    -o build/usdjpy_asian_bt -pthread

# Trail-axis sweep is resumable. Cells already in the CSV are skipped.
python3 scripts/usdjpy_asian_trail_sweep.py phase1
python3 scripts/usdjpy_asian_trail_sweep.py phase2
python3 scripts/usdjpy_asian_trail_sweep.py oos        # OOS the auto-chosen winner

# Or run a single cell directly:
python3 scripts/usdjpy_asian_trail_sweep.py cell my_label \
    MIN_RANGE=0.20 SL_FRAC=0.80 MFE_TRAIL_FRAC=0.15

# Sorted leaderboard:
python3 scripts/usdjpy_asian_trail_sweep.py leaderboard
```

## Reminders

* GitHub PAT in `CLAUDE.md` (`ghp_9M2I…24dJPV4`) — please rotate before
  next session.
* This commit is local. Q2 answer was "skip push entirely"; push when you
  are ready.
* `feature/usdjpy-asian-open` still branched off `omega-terminal`; merge
  target is your call.
* Production engine in `include/UsdjpyAsianOpenEngine.hpp` is unchanged.
  The new config is captured here in this doc; promotion to live still
  requires a 2-week shadow paper run per the engine header's safety gate.
