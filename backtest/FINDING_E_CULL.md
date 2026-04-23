# Finding E — CULLED S17

## Status
**REJECTED.** Bracket bias propagation gate across the 16 non-gated engines in
`GoldEngineStack.hpp` is **not justified by data** and will not be applied.

## Hypothesis (S16)
On 2026-04-22, a cluster of bad LONG entries during a daylong downtrend
produced a substantial bleed. S16 hypothesised that propagating
`bracket_trend_bias()` (already accessible via `BracketTrendState.hpp`)
into the 16 ungated stack engines would have blocked those entries and
saved $55–$72 of the day's loss.

## Validation (S17)

### Data
Source: `logs/omega_2026-04-22.log` (24 MB, commit `c953d9c`).

Extracted via `finding_e_extract_v2.py`:
- **Bracket exits (trend state source):** 4
    - 00:56:29 LONG WIN (TRAIL_HIT)
    - 08:49:43 LONG LOSS (BREAKOUT_FAIL)
    - 17:40:32 SHORT LOSS (SL_HIT)
    - 17:42:47 LONG LOSS (SL_HIT)
- **GoldEngineStack entries (Finding E targets):** 10 unique
    - 07:00:26 AsianRange LONG (+3.60)
    - 12:42:24 TurtleTick SHORT (+1.44)
    - 13:40:13 VWAPStretchReversion LONG (+0.72)
    - 13:46:44 TurtleTick LONG (-10.56)
    - 14:03:15 VWAPStretchReversion LONG (-15.84)
    - 14:05:36 SessionMomentum SHORT (-5.28)
    - 14:32:30 TurtleTick SHORT (+2.16)
    - 15:00:00 LondonFixMomentum SHORT (-1.38)
    - 16:41:01 VWAPStretchReversion LONG (-7.40)
    - 20:34:47 DXYDivergence LONG (-12.50)
    - **Total: -$45.04**

### Result
`finding_e_validate` output (built from `finding_e_validate.cpp`):
```
engine                          side  tot   block   blk_pnl     nblk    nblk_pnl
AsianRange                      LONG  1     0       0.00        1        3.60
DXYDivergence                   LONG  1     0       0.00        1      -12.50
LondonFixMomentum               SHORT 1     0       0.00        1       -1.38
SessionMomentum                 SHORT 1     0       0.00        1       -5.28
TurtleTick                      LONG  1     0       0.00        1      -10.56
TurtleTick                      SHORT 2     0       0.00        2        3.60
VWAPStretchReversion            LONG  3     0       0.00        3      -22.52
TOTAL                                 10    0       0.00        10     -45.04
```

**`blocked_pnl = $0.00` across every engine. Zero entries would have been
blocked.**

### Why the gate never fires

`BracketTrendState::on_exit` activates bias only on:
- 2 consecutive same-direction WINS (long bias), OR
- 2 consecutive same-direction LOSSES, OR
- 2 same-direction LOSSES within a 30-minute window.

On 04-22, the bracket engines produced just 4 exits: 1 WIN + 3 LOSSES, and
the losses were **split across directions** (LONG, SHORT, LONG). The
17:40 SHORT loss broke any accumulating LONG loss streak, and no same-dir
2-loss pattern ever formed.

Bias was 0 for all 86,400 seconds of the day. Every stack entry PASSED.

## Decision
- The gate is correctly implemented in `BracketTrendState.hpp` (Layer 4 /
  Batch 4C verified).
- The 7 engines already wired (EMACross, GoldFlow, MacroCrash,
  SessionMomentum, TurtleTick, VWAPStretchReversion, plus shelved
  CompressionBreakout-in-stack) retain their gates. Nothing is being
  reverted.
- The proposed propagation of the gate into the remaining 16 engines is
  **not applied.** The target day's data shows the gate would have had
  zero effect; applying a gate that fires 0× per day adds complexity
  without measurable benefit.
- Walk-forward across additional days is plausibly useful but **not a
  prerequisite to the S17 decision**. The Apr 22 bleed that motivated
  Finding E was not a trend-bias problem — the bracket simply didn't
  produce enough same-dir losses to activate the gate. Different
  mechanism. Different patch. Different session.

## Artifacts preserved in this directory
Kept for audit / future re-evaluation:
- `finding_e_extract_v2.py`  — log-to-CSV extractor (SHADOW-CLOSE + GOLD-STACK-ENTRY joiner, ANSI-stripping, FIFO pnl pairing)
- `finding_e_bracket_exits_v2.csv`  — 4 bracket exits from 04-22
- `finding_e_all_entries_v2.csv`    — 10 stack entries from 04-22
- `finding_e_summary_v2.csv`        — validator per-engine summary (all blk_pnl = 0)
- `finding_e_trace_v2.csv`          — full bias trace (bias = 0 throughout)
- `logs/omega_2026-04-22.log`       — source log (committed S17 P2)

## Removed in this cull
- `finding_e_validate.cpp`  — validator binary's source (rendered obsolete; result is known and documented here)
- v1 extractor + CSVs (`finding_e_extract.py`, `finding_e_*.csv`) — superseded by v2 before any decision was made

## Follow-up for S18
If a future day DOES exhibit 2+ same-dir bracket losses and the ungated stack
engines fire LONG during that window, re-evaluate. Until then, the gate is
dormant by design on 04-22-like days and the propagation has no justified
target.

Session 17 — Finding E closed.
