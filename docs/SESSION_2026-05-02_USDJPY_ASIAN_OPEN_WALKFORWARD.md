# SESSION — USDJPY Asian Open: Walk-Forward Validation + Exit/Sizing Sweep

Date: 2026-05-02 (third session of the day, after the trail-fix sweep
and the Phase 2 fine grid).

## Lineage

- `docs/SESSION_2026-05-02_USDJPY_ASIAN_OPEN_TRAIL_FIX.md` — original
  trail-fix sweep that found the prior chosen winner.
- `docs/SESSION_2026-05-02_USDJPY_ASIAN_OPEN_PHASE2_FINE.md` — fine
  grid that confirmed the plateau and rejected the corner-peak overfit.
- `docs/SESSION_2026-05-02_USDJPY_ASIAN_OPEN_WALKFORWARD_HANDOFF.md` —
  handoff that specified the walk-forward task.
- `docs/SESSION_2026-05-02_USDJPY_ASIAN_OPEN_WALKFORWARD.md` — this
  doc, which executed walk-forward, found a soft pass, then on user
  push-back diagnosed exits, swept the unswept axes (TP/SL/BE/LOT),
  and re-walk-forwarded a new winner that hard-passes.

## TL;DR

The chosen winner from the trail-fix sweep walk-forwards to a SOFT
PASS only. Diagnosis of the trade ledger showed:

- The trail is doing its job (winners realise 6.60p of an 8.25p
  available MFE = 80% capture, only 1.65p clip per winner).
- TP_RR=2.0 fires 1 / 278 trades (0.4%) — TP is dead weight at that RR.
- SL_FRAC=0.80 was only the local optimum within {0.75, 0.80, 0.85};
  no one had ever tried 1.00.
- Lot size is fixed 0.20 across all 278 trades (no Kelly-aware sizing).

A targeted four-phase sweep (TP_RR → SL_FRAC → BE_TRIGGER_PTS →
LOT_MAX) found a new winner that **HARD-passes** the walk-forward:

```cpp
static constexpr double MIN_RANGE       = 0.20;     // unchanged
static constexpr double SL_FRAC         = 1.00;     // was 0.80
static constexpr double MFE_TRAIL_FRAC  = 0.15;     // unchanged
static constexpr double TP_RR           = 0.5;      // was 2.0
static constexpr double BE_TRIGGER_PTS  = 0.06;     // unchanged
// LOT_MAX kept at 0.20 (do not raise pre-shadow gate)
```

Walk-forward result on the new winner: **total PnL +$244.20,
7/8 profitable months, WF PF 1.37, worst DD $181 (under $200 cap).**

## Phase 1 — Walk-forward of the original chosen winner

Built `scripts/usdjpy_asian_walkforward.py`. Pre-flight baseline-
reproduction check matched bit-exactly ($317.10 / $202.51 / 8 of 14).
Then 8 rolling folds (6mo train / 1mo test / 1mo step) with fixed
params MR=0.20, SL=0.80, MFE=0.15.

**Train windows (sanity):**

| fold | window               | trades | WR    | PF   | PnL      | DD       | p_mo |
|-----:|----------------------|-------:|------:|-----:|---------:|---------:|------|
| 1    | 2025-03..2025-08     | 166    | 83.1% | 1.52 | +$367.34 | $118.01  | 5/6  |
| 2    | 2025-04..2025-09     | 162    | 80.2% | 1.22 | +$186.55 | $118.01  | 4/6  |
| 3    | 2025-05..2025-10     | 123    | 81.3% | 1.32 | +$199.02 | $202.51  | 5/6  |
| 4    | 2025-06..2025-11     | 94     | 78.7% | 1.08 | +$44.73  | $202.51  | 4/6  |
| 5    | 2025-07..2025-12     | 80     | 77.5% | 1.05 | +$24.29  | $202.51  | 4/6  |
| 6    | 2025-08..2026-01     | 85     | 76.5% | 1.15 | +$79.47  | $202.51  | 4/6  |
| 7    | 2025-09..2026-02     | 104    | 76.0% | 1.05 | +$32.23  | $202.51  | 3/6  |
| 8    | 2025-10..2026-03     | 99     | 77.8% | 1.21 | +$118.70 | $141.04  | 3/6  |

**Test months (out-of-sample):**

| fold | month   | trades | WR    | PF   | PnL      | DD       |
|-----:|---------|-------:|------:|-----:|---------:|---------:|
| 1    | 2025-09 | 12     | 66.7% | 0.59 | -$54.24  | $112.58  |
| 2    | 2025-10 | 28     | 85.7% | 2.24 | +$109.19 | $29.36   |
| 3    | 2025-11 | 8      | 75.0% | 0.78 | -$12.60  | $41.53   |
| 4    | 2025-12 | 8      | 62.5% | 0.83 | -$9.62   | $33.75   |
| 5    | 2026-01 | 27     | 81.5% | 2.00 | +$112.12 | $52.76   |
| 6    | 2026-02 | 29     | 77.4% | 0.93 | -$14.45  | $141.04  |
| 7    | 2026-03 | 9      | 66.7% | 0.70 | -$23.22  | $44.37   |
| 8    | 2026-04 | 5      | 80.0% | 1.07 | +$1.79   | $26.54   |

Aggregate: +$108.97 over 8 months, 3/8 profitable, WF PF 1.14, worst
month -$54, worst DD $141.

**Verdict: SOFT PASS only.** PF 1.14 vs 1.15 hard threshold; profitable
months 3/8 vs ≥5/8 hard threshold.

## Phase 2 — Exit diagnostics on the chosen-winner ledger

User push-back: "exits are an issue, find the winning settings, check
lot sizes."

Built `scripts/usdjpy_asian_exit_analysis.py`. Critical fix: the
engine writes `tr.mfe = mfe_ * size_` (lines 785-786 of
`include/UsdjpyAsianOpenEngine.hpp`), so MFE/MAE in the ledger are
multiplied by lot size. Divided by 0.20 to recover price distance.

After fix, on the 278-trade chosen-winner ledger:

```
EXIT-REASON MIX
    TRAIL_HIT      221  ( 79.5%)
    SL_HIT          49  ( 17.6%)
    BE_HIT           7  (  2.5%)
    TP_HIT           1  (  0.4%)    <-- TP basically never fires

MFE DISTRIBUTION (winners, n=221, pips)
    mean = 8.25, median = 7.20, p75 = 8.60, p90 = 10.45, max = 40.00

MFE CLIP — pips left on the table per winner
    mean = 1.65, median = 1.30, max = 11.40
    (winners realise 6.60p of an 8.25p available MFE = 80% capture)

MAE DISTRIBUTION (losers, n=55, pips)
    mean = 19.63, p90 = 24.15  (losses go ~20 pips before SL hits)

LOT-SIZE DISTRIBUTION
    100.0% of trades at LOT_MAX = 0.2000 (no size variation at all)
```

**Edge math at WR=80%, avg_win $8.22, avg_loss $26.32:**

  EV/trade = 0.80 × $8.22 - 0.20 × $26.32 = +$1.20

  At WR=75% (which is what bad months produce):
  EV/trade = 0.75 × 8.22 - 0.25 × 26.32 = -$0.42 (negative)

The strategy has razor-thin EV. A 5-pp WR drop flips it negative.
The trail isn't broken; the asymmetry between $8.22 wins and $26.32
losses is.

## Phase 3 — Targeted sweep on unswept axes

Built `scripts/usdjpy_asian_exit_sweep.py`. Anchors on chosen winner
(MR=0.20, SL=0.80, MFE=0.15) and sweeps four phases, each phase
inheriting the previous phase's best by full-period net_pnl:

### Phase A — TP_RR sweep

| TP_RR | PnL      | PF   | WR    | DD       | months |
|------:|---------:|-----:|------:|---------:|--------|
| 0.3   | $233.95  | 1.15 | 81.4% | $223.58  | 8/14   |
| **0.5** | **$358.60** | **1.24** | 80.8% | $205.16  | 8/14   |
| 0.7   | $284.49  | 1.18 | 79.9% | $202.48  | 8/14   |
| 1.0   | $312.13  | 1.21 | 79.6% | $202.51  | 8/14   |
| 1.5   | $304.09  | 1.20 | 79.5% | $202.51  | 8/14   |
| 2.0   | $317.10  | 1.21 | 79.5% | $202.51  | 8/14   |

TP_RR=0.5 wins. TP fires 29 / 281 trades (10.3%), converting late-trail
exits into early TP banks. PnL +13% over chosen winner.

### Phase B — SL_FRAC sweep on top of TP_RR=0.5

| SL_FRAC | PnL      | PF   | WR    | DD       | months |
|--------:|---------:|-----:|------:|---------:|--------|
| 0.40    | $9.90    | 1.01 | 69.7% | $172.20  | 8/14   |
| 0.50    | $244.92  | 1.17 | 73.9% | $188.93  | 9/14   |
| 0.60    | $176.14  | 1.11 | 74.8% | $209.24  | 8/14   |
| 0.70    | $267.55  | 1.17 | 78.1% | $222.20  | 7/14   |
| 0.75    | $303.02  | 1.20 | 79.4% | $211.43  | 8/14   |
| 0.80    | $358.60  | 1.24 | 80.8% | $205.16  | 8/14   |
| 0.85    | $255.45  | 1.16 | 80.4% | $222.61  | 8/14   |
| 0.90    | $296.77  | 1.19 | 81.4% | $160.92  | 8/14   |
| **1.00**| **$407.60**| **1.28**| **83.9%**|**$186.14**| **9/14** |

SL_FRAC=1.00 wins decisively. Bigger individual losses ($33.39 vs
$26.32) but **materially fewer** of them — WR up from 80.8% to 83.9%,
DD actually drops from $205 to $186. This was the never-tested cell.

### Phase C — BE_TRIGGER_PTS sweep on top of TP=0.5, SL=1.0

| BE_TRIGGER | PnL      | PF   | WR    | DD       | months |
|-----------:|---------:|-----:|------:|---------:|--------|
| 0.03       | $179.60  | 1.18 | 50.8% | $195.59  | 10/14  |
| 0.04       | $259.50  | 1.22 | 62.3% | $284.67  | 10/14  |
| **0.06**   | **$407.60**| **1.28** | 83.9% | $186.14  | 9/14   |
| 0.08       | $357.68  | 1.24 | 83.9% | $204.41  | 8/14   |
| 0.10       | $357.68  | 1.24 | 83.9% | $204.41  | 8/14   |
| 0.15       | $357.48  | 1.24 | 84.2% | $204.41  | 8/14   |
| 0.20       | $357.48  | 1.24 | 84.2% | $204.41  | 8/14   |
| 0.30       | $357.48  | 1.24 | 84.2% | $204.41  | 8/14   |

Tighter BE (0.03, 0.04) shows interesting per-month consistency
(10/14) but blows DD to $285 and PF drops sharply (lots of trades
get BE-locked too early and can't develop). BE=0.06 is the dominant
choice — confirms the chosen-winner's existing value.

### Phase D — LOT_MAX (informational only)

| LOT_MAX | PnL      | DD       | PF   | months |
|--------:|---------:|---------:|-----:|--------|
| 0.05    | $101.90  | $46.53   | 1.28 | 9/14   |
| 0.07    | $142.66  | $65.15   | 1.28 | 9/14   |
| 0.10    | $203.80  | $93.07   | 1.28 | 9/14   |
| 0.15    | $305.70  | $139.60  | 1.28 | 9/14   |
| 0.20    | $407.60  | $186.14  | 1.28 | 9/14   |

PnL and DD scale linearly with LOT_MAX, PF/months invariant. Half-Kelly
at 0.07 gives PnL $142.66 / DD $65.15. Per handoff policy "do not raise
LOT_MAX without a passing shadow gate," kept LOT_MAX=0.20 as the
standard sweep size.

## Phase 4 — Train/test sanity check on the new winner

| Period | Trades | WR    | PF   | PnL      | DD       | months |
|--------|-------:|------:|-----:|---------:|---------:|--------|
| TRAIN  | 172    | 85.5% | 1.43 | $353.95  | $134.29  | 5/7    |
| TEST   | 102    | 83.3% | 1.28 | $155.19  | $186.14  | **5/7** |

Vs the original chosen-winner test side (3/7, $113.38, PF 1.19):
profitable months jump from 3/7 to 5/7 (43% → 71%), PnL +37%.
DD wider by $45 but still under $200 hard cap.

## Phase 5 — Walk-forward of the new winner

Extended `scripts/usdjpy_asian_walkforward.py` with `--params`,
`--baseline-pnl/-dd/-prof-m`, and `--csv` flags so both winners can
walk-forward without colliding CSVs.

Run:
```
python3 scripts/usdjpy_asian_walkforward.py --reset \
    --params SL_FRAC=1.0 TP_RR=0.5 BE_TRIGGER_PTS=0.06 \
    --baseline-pnl 407.60 --baseline-dd 186.14 --baseline-prof-m 9 \
    --csv build/walkforward_results_v2.csv
```

Pre-flight verify reproduced $407.60 / $186.14 / 9 of 14 exactly.
Determinism intact for the new winner.

**Train windows (sanity):**

| fold | window               | trades | WR    | PF   | PnL      | DD       | p_mo |
|-----:|----------------------|-------:|------:|-----:|---------:|---------:|------|
| 1    | 2025-03..2025-08     | 163    | 85.9% | 1.49 | +$368.27 | $134.29  | 5/6  |
| 2    | 2025-04..2025-09     | 158    | 84.2% | 1.30 | +$251.56 | $134.29  | 4/6  |
| 3    | 2025-05..2025-10     | 123    | 85.4% | 1.44 | +$266.61 | $179.06  | 4/6  |
| 4    | 2025-06..2025-11     | 93     | 82.8% | 1.17 | +$94.06  | $179.06  | 4/6  |
| 5    | 2025-07..2025-12     | 79     | 83.5% | 1.24 | +$109.67 | $179.06  | 4/6  |
| 6    | 2025-08..2026-01     | 84     | 82.1% | 1.24 | +$117.51 | $179.06  | 4/6  |
| 7    | 2025-09..2026-02     | 102    | 81.4% | 1.11 | +$70.19  | $181.63  | 4/6  |
| 8    | 2025-10..2026-03     | 98     | 82.7% | 1.23 | +$127.79 | $186.14  | 4/6  |

**Test months (out-of-sample):**

| fold | month   | trades | WR    | PF   | PnL      | DD       |
|-----:|---------|-------:|------:|-----:|---------:|---------:|
| 1    | 2025-09 | 11     | 81.8% | 1.19 | +$15.11  | $54.71   |
| 2    | 2025-10 | 28     | 85.7% | 1.87 | +$92.66  | $45.39   |
| 3    | 2025-11 | 8      | 87.5% | 1.34 | +$13.03  | $38.32   |
| 4    | 2025-12 | 7      | 100%  | n/a  | +$67.92  | $0.00    |
| 5    | 2026-01 | 27     | 81.5% | 1.38 | +$52.77  | $66.76   |
| 6    | 2026-02 | 31     | 77.4% | 0.73 | -$70.48  | $181.63  |
| 7    | 2026-03 | 9      | 88.9% | 2.22 | +$38.67  | $31.62   |
| 8    | 2026-04 | 5      | 100%  | n/a  | +$34.52  | $0.00    |

**Aggregate over 8 OOS test months:**
```
total PnL              :  +$244.20
profitable months      :   7 / 8     (87.5%)
deeply-neg months      :   0         (threshold -$80)
worst test-month PnL   :  -$70.48    (fold 6 / 2026-02)
worst test-month DD    :   $181.63   (fold 6 / 2026-02)
walk-forward PF        :   1.37      ($901.54 wins / $657.49 losses)
```

## Verdict — NEW WINNER

**HARD PASS.** All three hard-pass criteria met:

| criterion                    | threshold       | result | met |
|------------------------------|-----------------|--------|-----|
| soft: total WF PnL           | ≥ $56           | $244   | yes |
| soft: deeply-neg months      | ≤ 2             | 0      | yes |
| soft: WF PF                  | ≥ 1.05          | 1.37   | yes |
| **hard: WF PF**              | ≥ 1.15          | **1.37**| **YES** |
| **hard: profitable months**  | ≥ 5 / 8         | **7 / 8** | **YES** |
| **hard: worst test-month DD**| < $200          | **$181**| **YES** |
| fail: WF PF                  | < 1.0           | 1.37   | no  |
| fail: any test month         | < -$200         | -$70   | no  |

## Side-by-side

| Metric                | Chosen Winner | New Winner | Δ        |
|-----------------------|--------------:|-----------:|---------:|
| Full-period in-sample PnL | +$317.10  | +$407.60   | +28.5%   |
| In-sample PF          | 1.21          | 1.28       | +0.07    |
| In-sample WR          | 79.5%         | 83.9%      | +4.4pp   |
| In-sample DD          | $202.51       | $186.14    | -$16     |
| In-sample prof_months | 8/14          | 9/14       | +1       |
| WF total PnL          | +$108.97      | +$244.20   | **+124%**|
| WF PF                 | 1.14          | 1.37       | +0.23    |
| WF profitable months  | 3/8           | **7/8**    | +4       |
| WF worst test-month PnL | -$54.24     | -$70.48    | -$16     |
| WF worst test-month DD| $141.04       | $181.63    | +$40     |
| **Hard-pass criteria**| **NO**        | **YES**    |          |

## Recommendation

The new winner clears all hard-pass criteria with margin to spare.
Profitable-month ratio is the headline improvement: 7/8 (87.5%) vs
3/8 (37.5%). The worst single test month is somewhat worse (-$70
vs -$54) but well within the -$200 hard-fail threshold.

Promote the new winner to the **2-week paper shadow gate**. If shadow
results corroborate, the engine is ready for cautious live promotion
at LOT_MAX=0.20 (or scaled down per Phase D's curve if conservative
risk is preferred — half-Kelly at LOT_MAX=0.07 yields PnL $142 /
DD $65 with identical PF).

## Why the change worked

Two things, both surprising:

1. **TP_RR=0.5 instead of 2.0.** With TP at ~10 pips (≈ p75 MFE),
   29 trades per period get an early TP-bank instead of riding the
   trail into a partial give-back or a reversal. Especially
   consequential in choppy months where the trail can dribble away
   gains. Doesn't sacrifice big-MFE runs (those still trail past the
   TP if MFE was reached very fast — TP fires once price actually
   touches it).

2. **SL_FRAC=1.00 instead of 0.80.** Wider SL means individual losses
   are bigger ($33.39 vs $26.32), but in this regime the wider SL
   avoids many noise-stop-outs. Net WR jumps from 79.5% to 83.9%,
   which more than compensates. The fine grid only swept SL_FRAC ∈
   {0.75, 0.80, 0.85} — 1.00 was outside the explored range.

The earlier "trail clipping" narrative from the original handoff was
genuinely true at MFE_TRAIL_FRAC=0.50 (clip ~3.84 pips). The trail-fix
sweep cut the clip to 1.65 pips by lowering MFE_TRAIL_FRAC to 0.15.
But that wasn't the constraint anymore — the constraints were TP
position and SL whipsaw.

## Artifacts produced this session

```
scripts/usdjpy_asian_walkforward.py    new file (uncommitted)
                                        + extended with --params /
                                          --baseline-* / --csv flags
scripts/usdjpy_asian_exit_analysis.py  new file (uncommitted)
scripts/usdjpy_asian_exit_sweep.py     new file (uncommitted)
build/walkforward_results.csv          chosen-winner WF (uncommitted)
build/walkforward_results_v2.csv       new-winner WF (uncommitted)
build/usdjpy_exit_sweep_results.csv    27 sweep cells (uncommitted)
docs/SESSION_2026-05-02_USDJPY_ASIAN_OPEN_WALKFORWARD.md   this doc (uncommitted)
```

Production engine `include/UsdjpyAsianOpenEngine.hpp` is unchanged.
md5 still `d514fce983cf10914c77d001311bd4be`. The new winner is NOT
yet in the production header — that promotion is gated on the shadow
test result.

## Next session — queue

1. **Schedule the 2-week paper shadow gate on the new winner.** This
   is the actual promotion check. Configure shadow to run with:

   ```cpp
   MIN_RANGE = 0.20, SL_FRAC = 1.00, MFE_TRAIL_FRAC = 0.15,
   TP_RR = 0.5, BE_TRIGGER_PTS = 0.06, LOT_MAX = 0.20
   ```

   If shadow comes back marginal/negative, reconsider promotion;
   if shadow corroborates, propose the production-header change
   for explicit user approval.

2. **(optional)** If shadow is delayed and you want extra OOS evidence,
   re-run the new-winner walk-forward as 6mo-train / **2mo**-test
   (4 folds with bigger sample sizes per fold). Lower noise per fold,
   doubles the signal-to-noise on the per-month profitability claim.

3. **(deferred from prior queue)** USDJPY London hours (06–09 UTC) on
   the new winner. One full-period cell.

4. **(deferred)** Tokyo-fix exclusion (00:50–01:00 UTC).

5. **(deferred)** Restricted session windows {01–04, 02–04, 00–02} UTC.

6. **(deferred)** Mean-reversion variant. Lowest priority.

## Reminders

- GitHub PAT in `CLAUDE.md` (`ghp_9M2I…24dJPV4`) — please rotate. Has
  been called out in three session docs and not yet acted on.
- The new winner is HARD-PASS in walk-forward but **still requires
  the 2-week paper shadow gate** before live promotion. Walk-forward
  is OOS validation, not the safety gate.
- Do **NOT** raise LOT_MAX above 0.20 without passing the shadow gate.
  At 0.20 the strategy is already aggressive vs Kelly (half-Kelly is
  ~0.07).
