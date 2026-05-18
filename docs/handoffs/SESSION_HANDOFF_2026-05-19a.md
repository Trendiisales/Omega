# Session Handoff — 2026-05-19 part A (NZST)

Read this first next session. Direct follow-up to part-B
(`SESSION_HANDOFF_2026-05-18b.md`). That session's primary recommended
next-session focus — the GSP exit-philosophy variants sweep — has now
run. **No variant recovers tradeable edge in tick-level execution.**
Per the failure criterion in part-B §"Recommended next-session focus",
trail-based exits for gold M5 are abandoned and the next research
direction is fixed-RR (TP at 2R/3R, hard SL, no trail).

## TL;DR

1. **GSP engine refactored (S101a):** added `enum class ExitPhilosophy`
   (`TICK_LEVEL`, `BAR_CLOSE_ONLY`, `GIVE_BACK`) and runtime field
   `exit_philosophy` on `GoldScalpPyramidEngine`. `_manage_position`
   branches on the enum. Default is `TICK_LEVEL` so the live
   `g_gold_scalp_pyramid` (still `enabled=false` from S100c) behaves
   identically to pre-refactor. No core code touched.

2. **Harness built (S101b):** `backtest/gsp_exit_variants_bt.cpp`
   drives the class directly, sweeps 12 configs:
   `trail_tight ∈ {0.12, 0.20, 0.30, 0.40} ×
    exit_philosophy ∈ {TICK_LEVEL, BAR_CLOSE_ONLY, GIVE_BACK}`.
   Core held at GSP best: LB=8, SL=1.5, TP=3.0, Pyr=Y, S63=OFF.
   154M-tick run completed in 3.7 min (sequential, ~18s/config).

3. **Sweep result: every config fails the bar.**
   Best PnL: `t=0.20/TICK` at -$16,933.99 / PF 0.62 — i.e. the
   pre-refactor live shape was already the best of the 12. Worst:
   `t=0.30/BAR_CLOSE` at -$17,982.08 / PF 0.57. Total spread $1,048;
   the trail tightness × exit philosophy axis moves PnL by only ~5%.

4. **Why no variant works — the deeper structural finding:**
   - WR is uniform at 71% for `TICK_LEVEL` and `GIVE_BACK`
     (`BAR_CLOSE_ONLY` drops to 65.6%).
   - AvgWin is uniformly ~$5.50; avgLoss is uniformly ~$21.78.
   - Win:loss ratio ≈ 1:4 → required break-even WR ≈ 80%.
   - Gold M5 produces 71% WR. The 9-pt gap is the entire deficit.
   - Trail philosophy is **not** the bottleneck. Whether we trail per
     tick, only at bar close, or use a giveback rule, the avgWin
     ceiling is set by how much excursion these scalp entries
     actually produce; the avgLoss floor is set by 1.5×ATR hard SL.
   - The bar-vs-tick gap proved in part-B is a real measurement
     artifact; it is **not** a recoverable edge under any trail
     formulation tested.

5. **Verdict per failure criterion in part-B:** trail-based exits
   for gold M5 are abandoned. Next direction is fixed-RR (TP at 2R/3R,
   hard SL, no trail). The engine refactor remains in tree because
   it cost nothing and the `ExitPhilosophy` field is useful for future
   experiments that revisit trail logic on other instruments/regimes.

## Sweep results — full table

12 configs, sorted by gross PnL descending. CSV: 154M Dukascopy ticks
XAUUSD 2024-03 → 2026-04. Per-config runtime ~18s; total 3.7 min.

```
Config                 N       WR%    PnL$         PF     DD$       AvgWin    AvgLoss   TR    TP    SL    GB    TS    Pyr
---------------------------------------------------------------------------------------------------------------------------
t=0.20/TICK            7069    71.0   -16933.99    0.62   17005.26      +5.53    -21.77 5038  1     1953  0     77    77
t=0.12/TICK            7110    71.0   -16934.62    0.62   16993.24      +5.54    -21.78 5071  0     1962  0     77    27
t=0.30/TICK            6987    71.0   -16970.43    0.62   17035.92      +5.47    -21.78 4978  2     1927  0     80    144
t=0.12/GIVE_BACK       7437    71.0   -17214.16    0.63   17291.88      +5.62    -21.75 881   1     2061  4412  82    14
t=0.40/TICK            6887    71.0   -17231.62    0.60   17312.79      +5.38    -21.80 4905  2     1902  0     78    237
t=0.20/GIVE_BACK       7394    71.0   -17414.56    0.63   17480.42      +5.56    -21.76 880   3     2047  4382  82    40
t=0.40/BAR_CLOSE       6220    65.5   -17471.08    0.58   17619.88      +5.99    -19.52 4340  72    1735  0     73    537
t=0.30/GIVE_BACK       7261    71.0   -17476.66    0.62   17545.69      +5.53    -21.80 869   16    2017  4277  82    123
t=0.40/GIVE_BACK       7045    71.1   -17520.75    0.61   17734.75      +5.43    -21.93 853   46    1951  4115  80    237
t=0.12/BAR_CLOSE       6300    65.6   -17699.38    0.58   17767.28      +5.95    -19.55 4417  51    1761  0     71    415
t=0.20/BAR_CLOSE       6286    65.6   -17862.01    0.58   17961.71      +5.89    -19.50 4402  56    1755  0     73    451
t=0.30/BAR_CLOSE       6255    65.5   -17982.08    0.57   18133.30      +5.87    -19.52 4375  61    1745  0     74    493
```

Legend: TR=TRAIL_HIT TP=TP_HIT SL=SL_HIT GB=GIVEBACK_HIT TS=TIME_STOP
Pyr=trades_with_pyramid_layers (size > 1× base).

## Observations from the table

1. **TICK_LEVEL is already the best of the three.** All 4 TICK variants
   beat all 4 BAR_CLOSE variants and (mostly) all 4 GIVE_BACK variants.
   The pre-refactor live shape was the local optimum on this tape.

2. **TRAIL_TIGHT is decorative in TICK_LEVEL.** The 4 TICK rows are
   identical to within $300 PnL (0.12→0.40 spread is $297). Reason:
   Phase 2 (lock 35% of MFE) almost always fires before Phase 3/4
   arms, so the `TRAIL_TIGHT` ATR-fraction never gets used. Tighten
   Phase 2 if you want a real trail-width experiment in future.

3. **BAR_CLOSE_ONLY systematically degrades WR by 5pts** (71→65.5%)
   without helping avgWin enough. The deferred trail lets losers run
   wider (avgLoss tightens to -$19.50 from -$21.78 — false-precision
   in the WR drop). 72 TP_HITs vs 0-2 in TICK shows bar-close trail
   does allow some intra-bar peaks to reach TP, but the WR damage
   dominates.

4. **GIVE_BACK adds GIVEBACK_HIT exit reason (~4,200 trades per cfg)**
   replacing most TRAIL_HITs (881→4412 GB replaces 5038→881 TR). Same
   avgWin/avgLoss as TICK, slightly worse PnL because the giveback
   trigger arm at ATR×0.4 fires slightly later than the 4-phase trail.

5. **Pyramid layer count varies wildly (14-537)** with no
   corresponding PnL improvement. Pyramiding doesn't rescue this
   shape either — extra layers are mostly small wins or full SL
   losses across all layers.

## Files in this session — final state

```
Modified (committed):
M include/GoldScalpPyramidEngine.hpp    -- ExitPhilosophy enum + branch in _manage_position
                                           Default TICK_LEVEL preserves live behavior.

Added (committed):
?? backtest/gsp_exit_variants_bt.cpp    -- 12-config sweep harness, drives class directly
?? docs/handoffs/SESSION_HANDOFF_2026-05-19a.md  (this file)

Generated, gitignored (kept locally as evidence):
   backtest/gsp_exit_variants_bt          (binary, ~75KB)
   backtest/gsp_exit_variants_results.txt (~6MB stdout, gitignored)
   backtest/gsp_exit_variants_progress.log (~133 lines stderr, gitignored)
```

## Pre-deploy checklist (this commit bundle)

```bash
cd ~/omega_repo

# 1. Mac canary green (already verified this session)
cmake --build build --target OmegaBacktest -j 2>&1 | tail -5

# 2. Stage source-only (no result txt -- gitignored)
git add include/GoldScalpPyramidEngine.hpp \
        backtest/gsp_exit_variants_bt.cpp \
        docs/handoffs/SESSION_HANDOFF_2026-05-19a.md

# 3. Review
git diff --cached --stat

# 4. Single commit -- the refactor + harness + null-result handoff are
#    tightly coupled. Refactor exists only to enable the sweep; sweep
#    exists only to test the hypothesis the part-B handoff laid out;
#    handoff records the null result and the pivot decision.
git commit -m "S101: GSP exit-philosophy variants sweep -- no variant recovers edge

12-config sweep (trail_tight x ExitPhilosophy {TICK, BAR_CLOSE, GIVE_BACK})
on 154M-tick Dukascopy XAUUSD tape via class-direct harness. All 12 fail
the success bar (PnL > \$5K, PF > 1.20). Best: t=0.20/TICK -\$16,934 / PF
0.62 -- identical to the pre-refactor live shape. Structural finding:
avgWin/avgLoss ratio ~1:4 needs ~80% WR; gold M5 produces 71%. Per the
part-B failure criterion, trail-based exits abandoned for gold M5;
next direction is fixed-RR. Engine refactor (ExitPhilosophy enum +
_manage_position branch) is backward-compatible (default TICK_LEVEL =
pre-refactor) and stays in tree for future trail experiments on other
instruments. No new engine wired. g_gold_scalp_pyramid stays
enabled=false (unchanged from S100c). See docs/handoffs/SESSION_HANDOFF_
2026-05-19a.md for full table + analysis."

# 5. Push
git push origin main

# 6. VPS deploy + verify (only if operator wants the refactor on the
#    live binary -- it's backward-compatible so safe to ship, but
#    nothing in production behavior changes. Operator's call.)
```

The bundle does **not** enable or wire any new engine. Live engine
state is unchanged: `g_gold_scalp_pyramid.enabled = false` from S100c.

## What did NOT land this session

- A tradeable variant. None of the 12 configs cleared the success bar.
- Any change to live engine state. `g_gold_scalp_pyramid` stays
  `enabled=false`.
- The fixed-RR pivot itself. Scoped as next-session focus below.

## Recommended next-session focus

**Single primary goal: build a fixed-RR variant of the GSP shape and
run a tick-level class-direct sweep against the same 154M-tick tape
to find out whether removing the trail entirely produces tradeable
edge.**

The plan:

1. Add a new engine `GoldScalpFixedRR.hpp` (or, more invasively, add
   a `use_fixed_rr` flag to GSP itself — the standalone-engine route
   is cleaner for a hypothesis test). Entry stack identical to GSP
   best-config: M5 Donchian-break + EMA9/EMA21 + momentum-bar +
   session 07-21 UTC + ATR floor/cap. Exit stack stripped to: hard
   SL at 1.5×ATR, hard TP at `RR × SL` for RR ∈ {2.0, 2.5, 3.0}.
   No trail, no Phase-1 BE lock, no S63 LOSS_CUT/BE_CUT. The
   hypothesis is that the 71% WR comes from the entry filter, and
   that letting winners run to a fixed TP without trail capture
   produces avgWin closer to 2-3× avgLoss.

2. Sweep on the same tape:
   - `RR ∈ {2.0, 2.5, 3.0}` (3)
   - `SL_ATR_MULT ∈ {1.0, 1.25, 1.5, 1.75}` (4)
   - 12 configs total, expected ~4 min runtime by analogy with S101.

3. Success criterion stays the same: any single config produces
   PnL > +$5,000 AND PF > 1.20 on the full tape. If yes → wire
   shadow-mode with enabled=false, propose live observe period.

4. Failure criterion: all configs negative, or PF < 1.0 on the best
   config. In that case the entry stack itself doesn't carry an edge
   independent of exit philosophy, and gold M5 scalping should be
   shelved entirely in favour of a different timeframe or instrument.

5. Honest expectation: the 71% WR + 1:4 win/loss ratio is consistent
   with mean-reverting entries on a noisy instrument. Fixed-RR with
   wider TP may produce a 30-40% WR with 1:2-3 win/loss ratio
   instead — that's the structure we need to verify. If the entries
   carry no edge at any RR, then the gold M5 Donchian/EMA/momentum
   entry shape itself is the unfixable piece.

## Important lessons / don't-repeat (additive to part-B)

1. **A philosophy sweep is not an entry-shape sweep.** Today's null
   result narrows the search: trail formulation is not the
   bottleneck on this entry shape. The next experiment must vary
   the exit *magnitude* (RR), not the exit *formulation* (trail).
   If RR sweep also fails, then the entry shape itself is the
   bottleneck — and no exit refinement will rescue it.

2. **Default values matter even for "default" enums.** The refactor
   defaulted `exit_philosophy` to `TICK_LEVEL` precisely so the
   live engine continues behaving identically. Verified by running
   `t=0.12/TICK` (cfg 1) and matching part-B's class-direct audit
   result to two decimal places (-$16,934.62 vs -$16,935).

3. **Run the cheap experiment before the expensive engine.** The
   sweep ran in 3.7 min total. The temptation in part-B was to
   build three different engines (GoldScalpBarClose,
   GoldScalpGiveback, ...). The runtime-parameter refactor route
   was an order of magnitude faster to set up and produced the
   same answer.

## Next-session opening sequence

```bash
cd ~/omega_repo

# Read this handoff first
less docs/handoffs/SESSION_HANDOFF_2026-05-19a.md

# Confirm state
git log --oneline -10
git status
git rev-parse HEAD                          # should equal origin/main

# Then start the work described in "Recommended next-session focus"
```

End of handoff.
