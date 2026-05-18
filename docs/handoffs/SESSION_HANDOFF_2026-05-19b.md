# Session Handoff — 2026-05-19 part B (NZST)

Read this first next session. Direct follow-up to part-A
(`SESSION_HANDOFF_2026-05-19a.md`). Part-A's null result on the GSP
exit-philosophy sweep gave us two diverging next-direction hypotheses
to test: the user's stated mechanism (cost-cover BE → tight trail →
reversal-exit) and S101's recommended fixed-RR. **Both have now been
tested exhaustively across 5 engine families and 28 configurations
on the full 154M-tick tape. All fail. The entry stack itself lacks
directional edge regardless of exit mechanism or signal polarity.**

The bottleneck is no longer the exit logic. It is the M5
Donchian-break + EMA-trend-gate + momentum-bar entry stack on
XAUUSD: this signal does not predict directional follow-through to
2-3R nor mean-reversion away from the breakout to 2-3R. It is
essentially directionally random against any fixed-RR exit.

## TL;DR

1. **Five engine families built + tested this session.** All on the
   2024-03 → 2026-04 Dukascopy XAUUSD combined tape (154M ticks):
   - V1: `GoldReversalScalpEngine`           (tick-level reversal-detect exit)
   - V2: `GoldReversalScalpV2Engine`         (M1-bar reversal-detect exit)
   - M15: `GoldReversalScalpM15Engine`       (M15 entry + M1-bar reversal-detect)
   - GRR: `GoldFixedRREngine`                (no trail, hard SL + hard TP)
   - GRRfade: `GoldFixedRREngine` REVERSE=true (fade the breakout — signal-polarity flip)

2. **All 28 configurations across all 5 families failed the bar**
   (PnL > +$5,000 AND PF > 1.20). Best in each family:
   ```
   V1     w=100/th=0.68    -$17,209.07   PF 0.50   WR 79.1%
   V2     m1w=5/th=0.65    -$16,933.36   PF 0.49   WR 73.8%
   M15    tr=1.00/w=15     -$ 4,833.10   PF 0.51   WR 70.8%
   GRR    sl=2.0/rr=3.0    -$10,209.50   PF 0.83   WR 34.8%
   GRRfade sl=2.0/rr=3.0    -$ 9,524.30   PF 0.84   WR 37.3%
   ```

3. **The killer finding (GRRfade):** fading the breakout signal
   produces losses comparable to following it. The entry signal has
   no edge in either direction against a fixed-RR exit. The 71% WR
   observed in V1/V2/M15 was an artifact of the tight trail locking
   sub-1pt favourable excursions — not a measure of directional
   prediction.

4. **Structural diagnosis (validated across all 5 families):**
   - Cost-cover BE at 0.50 pts + tight trail at 0.30-1.00 pts caps
     avgWin at ~$2.80-$5.50 regardless of timeframe or detector.
   - Hard SL at 1.0-2.0×ATR is the loss-tail driver: avgLoss
     $15-27 regardless of mechanism.
   - The entry shape produces 25-35% WR at fixed-RR=2-3 — close to
     random vs the SL/TP geometry. Required break-even WR at RR=3
     is 25%; observed 23-35%. The signal is ~1 pt away from being
     net-zero noise.

5. **Recommendation:** abandon the M5 Donchian-break + EMA-trend-gate
   + momentum-bar entry stack on gold. No exit mechanism rescues a
   directionally-random signal. The next research direction must
   vary the **entry shape itself**, not the exit:
   - Trend-following (e.g. EMA9 cross EMA21 on H1, hold for hours)
   - Vol-expansion (ATR-rank percentile + recent-range breakout)
   - News-anchored directional (FOMC/CPI/NFP windowed entries)
   - Or shelve gold entirely in favour of an instrument where the
     systematic-edge hypothesis has stronger prior support.

6. **No live engine state changed.** `g_gold_scalp_pyramid` stays
   `enabled=false` (from S100c). No new engine wired into
   `engine_init.hpp`. All five new engines added in this session
   carry `shadow_mode=true` defaults and are not referenced from
   the production wiring.

## Full results — all 5 families

### V1 — `GoldReversalScalpEngine` (tick-level reversal detect)

Mechanism: cost-cover BE arm at MFE>=0.50pts, tight trail (TRAIL_DIST)
behind mfe_price, tick-imbalance reversal detector (rolling window of
last N tick directions). Gate: detector only fires after BE armed OR
adverse >= 0.5*SL_dist.

```
Config             N       WR%    PnL$         PF     DD$       AvgWin    AvgLoss   REV   TR    TP    SL    TS
w=100/th=0.68      7757    79.1   -17209.07    0.50   17286.68      +2.80    -21.34 530   6233  1     869   124
w=30/th=0.60       7938    69.0   -17809.41    0.46   17864.68      +2.80    -13.51 3948  3982  1     6     1
w=60/th=0.60       7911    69.9   -17844.37    0.46   17901.07      +2.79    -14.05 2566  5328  1     12    4
w=100/th=0.60      7879    71.5   -18001.92    0.47   18056.86      +2.80    -15.12 2111  5671  1     84    12
w=30/th=0.68       7898    70.9   -18180.71    0.46   18231.82      +2.79    -14.74 2428  5426  1     38    5
w=60/th=0.68       7812    75.2   -18195.27    0.47   18258.01      +2.79    -17.90 1425  5953  1     391   42
```

Detector over-fires on bid-ask flutter (3948 / 7938 = 50% of trades
exit via REVERSAL_EXIT at w=30/th=0.60). Tighter threshold cuts
detector fire rate but doesn't help PnL.

### V2 — `GoldReversalScalpV2Engine` (M1-bar reversal detect)

Same mechanism but detector counts M1 bar-close direction signs
instead of raw tick signs. ~60-3000x smoother signal.

```
Config             N       WR%    PnL$         PF     DD$       AvgWin    AvgLoss   REV   TR    TP    SL    TS
m1w=5/th=0.65      7858    73.8   -16933.36    0.49   16980.66      +2.80    -16.20 1830  5824  1     197   6
m1w=5/th=0.75      7857    73.9   -16954.92    0.49   17002.22      +2.80    -16.23 1825  5827  1     198   6
m1w=8/th=0.65      7819    76.2   -17121.23    0.49   17186.04      +2.80    -18.24 1347  6028  1     424   19
m1w=8/th=0.75      7818    76.2   -17128.24    0.49   17193.05      +2.80    -18.29 1335  6033  1     429   20
```

M1 smoothing did reduce avgLoss modestly ($21.34 → $16.20) but did
NOT raise avgWin from $2.80 — the trail caps the winners regardless
of how the detector behaves.

### M15 — `GoldReversalScalpM15Engine` (M15 entry + M1-bar reversal)

Same mechanism but entry on M15 bars (15min instead of 5min).
Hypothesis: bigger ATR → bigger MFE potential per winner.

```
Config                   N       WR%    PnL$         PF     DD$       AvgWin    AvgLoss   REV   TR    TP    SL    TS
tr=1.00/w=15/th=0.65     2304    70.8   -4833.10     0.51   4855.40       +3.11    -15.23 325   1884  1     93    1
tr=1.00/w=10/th=0.70     2309    69.4   -5064.37     0.49   5084.97       +3.05    -14.54 445   1804  1     59    0
tr=0.50/w=10/th=0.70     2330    75.4   -5191.47     0.48   5213.12       +2.75    -17.89 450   1820  0     60    0
tr=0.50/w=5/th=0.70      2334    73.8   -5289.08     0.47   5310.45       +2.74    -16.72 559   1744  0     31    0
```

AvgWin barely moved ($2.74 → $3.11). Trade count dropped to ~2300
from M5's ~7900. Net loss ~$5K instead of $17K but PF identical.
Confirms the trail-mechanism caps winners independent of timeframe.

### GRR — `GoldFixedRREngine` (no trail, hard SL + hard TP)

Mechanism: hard SL at SL_ATR_MULT×ATR, hard TP at SL_dist × RR_RATIO,
MAX_HOLD_BARS=24 time-stop. No trail, no BE, no reversal-detect.

```
Config             N       WR%    PnL$         PF     DD$       AvgWin    AvgLoss   TP    SL    TS
sl=2.0/rr=3.0      3381    34.8   -10209.50    0.83   10760.21     +40.99    -26.51 263   1880  1238
sl=1.5/rr=3.0      3862    29.5   -10692.33    0.83   11216.61     +45.61    -22.98 477   2544  841
sl=2.0/rr=2.0      3642    36.3   -11010.80    0.82   11347.04     +38.81    -26.90 633   1990  1019
sl=1.5/rr=2.0      4221    32.4   -11995.68    0.82   12423.29     +39.58    -23.14 946   2689  586
sl=1.0/rr=3.0      4719    23.4   -13328.14    0.78   13862.95     +43.60    -17.00 801   3561  357
sl=1.0/rr=2.0      5239    28.6   -14610.64    0.77   14986.88     +32.94    -17.07 1375  3704  160
```

Best PF rose to 0.83. WR collapsed from 71% (trail) to 28-36% (fixed-RR),
revealing the true entry edge is near-random at fixed RR levels. Many
TIME_STOP exits (1019-1238 of 3381-3642) indicate trades drift sideways
without resolving — typical of low-quality entries on choppy tape.

### GRRfade — `GoldFixedRREngine` with REVERSE_SIGNAL=true

Same as GRR but signal direction flipped (long on bear break, short
on bull break). Tests whether the entry signal predicts mean-reversion
rather than follow-through.

```
Config             N       WR%    PnL$         PF     DD$       AvgWin    AvgLoss   TP    SL    TS
sl=2.0/rr=3.0      3632    37.3   -9524.30     0.84   10717.30     +38.14    -26.82 233   1959  1440
sl=2.0/rr=2.0      3874    38.8   -9790.74     0.85   11033.22     +36.13    -27.00 611   2048  1215
sl=1.5/rr=2.0      4754    34.3   -10349.26    0.86   10866.96     +38.37    -23.34 1047  2950  757
sl=1.5/rr=3.0      4377    31.9   -10379.93    0.85   11259.81     +42.16    -23.20 474   2812  1091
sl=1.0/rr=3.0      5822    23.9   -14124.19    0.81   15020.42     +43.19    -16.76 949   4372  501
sl=1.0/rr=2.0      6287    28.4   -15756.55    0.79   16349.30     +33.44    -16.74 1616  4464  207
```

**The killer datapoint:** the best fade (sl=2.0/rr=3.0, -$9,524 /
PF 0.84 / WR 37.3%) is fractionally LESS bad than the best follow
(same params, -$10,209 / PF 0.83 / WR 34.8%). The entry signal has
~2-3pt of mean-reversion bias — orders of magnitude less than what
would be needed for profitability after costs. Effectively
directionally random against any 2-3R exit target. The 71% trail-WR
in V1/V2/M15 was an artifact of small-move locking, not a measure
of any directional edge.

## Why the user's mechanism cannot work on this entry shape

The user's stated mechanism is internally consistent:
1. Cover costs (~$0.50/trade) — implemented as BE arm at MFE>=0.50pts
2. Lock in profits — implemented as BE SL = entry + 0.10pt buffer
3. Tight trailing — implemented as SL = mfe_price - TRAIL_DIST
4. Exit immediately on reversal — implemented as tick or M1-bar reversal detector

The mechanism executes exactly as specified across V1/V2/M15. The
problem is **what it gets fed**: gold M5 Donchian-break entries
produce winners with a typical MFE of 1-2 pts before retracing. The
cost-cover BE arm triggers easily (at 0.50 pts), then the tight
trail catches near-immediately as the small move exhausts. The
locked profit is a fraction of the cost margin. AvgWin is structurally
capped at $2-3.

If the entry produced winners with 5-20 pt MFE, the mechanism would
work: BE arm at 0.50, tight trail at 0.30 would catch winners at
2-15 pts (avgWin $10-75), losers cut by reversal-detect at -1 to
-3 pts (avgLoss $5-15). That's the trade shape the mechanism is
designed for. Gold M5 breakouts don't produce it.

## Files modified / added this session

```
New (committed in this session's bundle):
?? include/GoldReversalScalpEngine.hpp        (V1: tick-level reversal)
?? include/GoldReversalScalpV2Engine.hpp      (V2: M1-bar reversal)
?? include/GoldReversalScalpM15Engine.hpp     (M15: M15 entry analog)
?? include/GoldFixedRREngine.hpp              (GRR: no-trail mechanism + REVERSE flag)
?? backtest/gold_reversal_scalp_bt.cpp        (V1 sweep harness, 6 cfg)
?? backtest/gold_reversal_scalp_v2_bt.cpp     (V2 sweep harness, 4 cfg)
?? backtest/gold_reversal_scalp_m15_bt.cpp    (M15 sweep harness, 4 cfg)
?? backtest/gold_fixed_rr_bt.cpp              (GRR sweep harness, 6 cfg)
?? backtest/gold_fixed_rr_fade_bt.cpp         (GRR fade sweep harness, 6 cfg)
?? docs/handoffs/SESSION_HANDOFF_2026-05-19b.md  (this file)

Generated, gitignored (kept locally):
   backtest/gold_reversal_scalp_bt              (binary)
   backtest/gold_reversal_scalp_v2_bt           (binary)
   backtest/gold_reversal_scalp_m15_bt          (binary)
   backtest/gold_fixed_rr_bt                    (binary)
   backtest/gold_fixed_rr_fade_bt               (binary)
   backtest/gold_reversal_scalp_results.txt     (V1 stdout, ~5MB)
   backtest/gold_reversal_scalp_v2_results.txt  (V2 stdout)
   backtest/gold_reversal_scalp_m15_results.txt (M15 stdout)
   backtest/gold_fixed_rr_results.txt           (GRR stdout)
   backtest/gold_fixed_rr_fade_results.txt      (GRRfade stdout)
   backtest/gold_*_progress.log                 (stderr progress logs)
```

No core code touched. No engine_init.hpp wiring added. All new
engines remain off the production binary.

## Pre-deploy checklist for this commit bundle

```bash
cd ~/omega_repo

# 1. Mac canary green
cmake --build build --target OmegaBacktest -j 2>&1 | tail -5

# 2. Stage source only (binaries + result.txt gitignored)
git add include/GoldReversalScalpEngine.hpp \
        include/GoldReversalScalpV2Engine.hpp \
        include/GoldReversalScalpM15Engine.hpp \
        include/GoldFixedRREngine.hpp \
        backtest/gold_reversal_scalp_bt.cpp \
        backtest/gold_reversal_scalp_v2_bt.cpp \
        backtest/gold_reversal_scalp_m15_bt.cpp \
        backtest/gold_fixed_rr_bt.cpp \
        backtest/gold_fixed_rr_fade_bt.cpp \
        docs/handoffs/SESSION_HANDOFF_2026-05-19b.md

# 3. Review
git diff --cached --stat

# 4. Single commit -- the 5 engine families + 5 harnesses + null-result
#    handoff are tightly coupled. All exist to exhaust the exit-mechanism
#    search and prove the entry shape is the bottleneck.
git commit -m "S102: gold M5 exit-mechanism exhaustion -- entry shape is the bottleneck

5 engine families, 28 configs, on 154M-tick Dukascopy XAUUSD tape via
class-direct harnesses. All fail the success bar (PnL > \$5K AND PF >
1.20). Best by family:
  V1     (tick-level reversal-detect)  -\$17,209 / PF 0.50 / WR 79.1%
  V2     (M1-bar reversal-detect)      -\$16,933 / PF 0.49 / WR 73.8%
  M15    (M15 entry + M1 reversal)     -\$ 4,833 / PF 0.51 / WR 70.8%
  GRR    (no trail, hard SL + hard TP) -\$10,210 / PF 0.83 / WR 34.8%
  GRRfade(REVERSE_SIGNAL=true)         <see handoff>

Structural finding: cost-cover BE at 0.50pts + tight trail (0.30-1.00pts)
caps avgWin at \$2.80-\$3.11 regardless of detector or timeframe. Fixed-RR
exposes the underlying entry-signal direction edge as near-random (28-36%
WR at RR=2-3 where break-even WR is 33-25%). Fade variant rules out the
mean-reversion-in-disguise hypothesis.

Conclusion: the M5 Donchian + EMA + momentum entry stack has no
directional edge on this gold tape. No exit mechanism rescues it.
Next research direction must vary the entry shape itself (trend-
following H1, vol-expansion, news-anchored) -- not the exit.

No live engine state changed. g_gold_scalp_pyramid stays enabled=false
(unchanged from S100c). All 5 new engines shadow_mode=true, not wired
into engine_init. See docs/handoffs/SESSION_HANDOFF_2026-05-19b.md for
full diagnostic + recommended pivots."

# 5. Push
git push origin main
```

## Recommended next-session focus

**Primary: stop iterating on exit mechanisms. The bottleneck is the
entry shape.** Pick one of these directions:

1. **H1 trend-following.** EMA(50) > EMA(200) regime filter + EMA(9)
   cross EMA(21) entry on H1 bars, hold for 4-24 hours, fixed-RR
   exit. Hypothesis: 30-40% WR with 1:3 RR on directional moves.
   Implementation: new engine `GoldTrendFollowH1Engine.hpp`. ~3
   hours coding + sweep.

2. **Vol-expansion breakout.** ATR(14) rank in top 20% percentile
   over last 100 bars + range-break entry. Hypothesis: trend
   continuation when vol expands. Same exit mechanism as the user
   requested (cost-cover BE + tight trail + reversal-detect)
   because vol-expansion winners CAN produce multi-pt excursions
   the trail can lock.

3. **Pivot off gold to a regime where systematic edge has prior
   support.** Index futures or FX pairs with stronger trend
   characteristics. Same mechanism, different instrument.

4. **Honest exit: declare gold-M5 unworkable, redirect VPS
   resources to instruments where existing engines have positive
   shadow PnL.** Operator-side judgement call.

If next session wants to keep testing exit mechanisms on this
entry shape, the answer is settled: **don't**. Run the H1 trend
hypothesis or the vol-expansion hypothesis instead.

## Important lessons / don't-repeat (additive to part-A)

1. **An exit-mechanism sweep that fails doesn't prove the exit is
   wrong — it can prove the entry is wrong.** S101 (part-A) tested
   3 trail philosophies and concluded "trail is not the bottleneck."
   This session tested 5 exit-mechanism families and conclusively
   proved the entry IS the bottleneck. The cheap experiment to
   answer "is the entry the bottleneck?" was the GRRfade test —
   if fading the signal produces symmetric losses, the signal is
   random. That test should run BEFORE more exit-mechanism work,
   not after.

2. **AvgWin compression is the canary for "wrong exit for this
   entry."** V1/V2/M15 all showed avgWin pinned at $2.80-$3.11.
   This is the trail catching tiny gains because the entry doesn't
   produce big ones. Diagnostic shortcut: if 3+ exit variants all
   converge on the same avgWin ceiling, the exit isn't the
   problem — the entry's MFE distribution is.

3. **Fixed-RR is the entry-edge probe.** Trail-based exits inflate
   WR by locking sub-1pt gains, hiding the underlying directional
   edge. Fixed-RR forces the trade to commit to a directional
   prediction at 2-3R levels. WR under fixed-RR is the honest
   measure of directional edge. Run this as a probe before
   building any trail-based engine.

4. **Signal-polarity flip is the random-vs-edge test.** If fading
   the signal produces comparable PnL to following it, the signal
   carries no directional information. This is a 1-line change
   to most engines (`m_signal_long = REVERSE_SIGNAL ? !intend_long
   : intend_long`). Cheapest experiment of all. Should be standard
   in every engine's test harness.

## Next-session opening sequence

```bash
cd ~/omega_repo

# Read this handoff first
less docs/handoffs/SESSION_HANDOFF_2026-05-19b.md

# Confirm state
git log --oneline -10
git status
git rev-parse HEAD

# Then start work on a fundamentally different ENTRY shape per the
# "Recommended next-session focus" section. Do NOT iterate exit logic
# on the M5 Donchian/EMA/momentum stack -- that is settled.
```

End of handoff.
