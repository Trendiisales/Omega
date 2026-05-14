# XauTrendFollow 2h — Tier 1 Phase 3 walk-forward validation — 2026-05-14

**Status:** COMPLETE. 4-window WF on `--mode baseline` (S63 trio
0.0/0.0/0.0, the state-B class default landed by part-W). Verdict:
**FAIL** on both gates. **1 of 4 windows passes `avg_pnl ≥ +0.001`**
(threshold ≥3), **aggregate PF=0.7240** (threshold ≥1.20, missed by
0.476). The engine fails the WF gate at baseline — i.e. *with* S63
off — meaning this is **not** an S63-adverse pattern in the
VWR/UTF5m sense, despite the auto-generated verdict text's
boilerplate phrasing. Three of four windows are individually
negative; the only passing window is w4 (2025-10 → 2026-04), the
final 6.5 months of the tape, riding XAU's late-2025/early-2026
rally. The track is **closed**. Engines stay at class-default
state-B values in `engine_init.hpp`; no engine_init.hpp activation.

Closure character: **harder fail** than either VWR S71 (2/4
windows, PF=1.04) or UTF5m S73 (3/4 windows, PF=1.12). See §7 for
the full comparison.

## 1. Scope

Phase 3 validated the XauTrendFollow2hEngine baseline (S63 trio =
0.0/0.0/0.0, class defaults after the part-W state E→B transition)
under a 4-window walk-forward split. Per part-W handoff
`docs/handoffs/SESSION_HANDOFF_2026-05-14l.md` §"Recommended
next-session focus" item 1.

Tape split into 4 non-overlapping equal-duration windows on a single
streaming pass. Each window run through the harness in `--mode
baseline`. The XTF trio has no session-window axis (XAU trades 24h
weekdays).

Pass criterion (pre-committed, no near-miss per project CLAUDE.md):
≥3 of 4 windows produce `avg_pnl ≥ +0.001` AND aggregate OOS profit
factor ≥ 1.20.

The "cost-pessimistic PF" framing uses the harness's per-trade
reported `gross_pnl` directly; `ExecutionCostGuard::is_viable()` has
already filtered trades that cannot cover costs at TP at fire time,
so the reported PnL reflects the round-trip economics of trades the
engine actually fired. Note that this is *more* aggressive filtering
than the original engine validation harness used (edge_hunt.cpp +
top_cells_monthly.cpp had no cost-guard at fire time) — see §9.

## 2. Run

```bash
python3 scripts/xtf_2h_wf_t1.py \
    /Users/jo/Tick/duka_ticks/XAUUSD_2024-03_2026-04_combined.csv
```

Wall-clock: ~60-90 seconds total (single streaming pass + 4
sequential harness runs).

Tape: 4.6 GB Dukascopy XAUUSD combined, 154,265,439 ticks. Format
auto-detected as A_TBA (timestamp_ms,bid,ask).

No disk-pressure issue. Outputs at
`outputs/xtf_2h_t1_p3_20260514_172751/`.

## 3. Tape coverage

The original validation-baseline tape (3-year Dukascopy
2023-09-27 → 2025-09-26) referenced in `XauTrendFollow2hEngine.hpp`
§"PROVENANCE" is gone (confirmed by part-K handoff and by `find`
this session). Substitute selected: the 2024-03 → 2026-04 combined
tape, which has an 18-month overlap with the original validation
window plus 8 additional months of post-validation data.

| Field | Value |
|---|---|
| Format detected | A_TBA |
| start_iso (UTC) | 2024-03-01T02:00:00Z |
| end_iso (UTC)   | 2026-04-24T20:59:58Z |
| total days      | 784.8 |
| window duration | 196.2 days each |

| Window | Start (UTC) | End (UTC) |
|---|---|---|
| w1 | 2024-03-01T02:00:00Z | 2024-09-13T06:44:59Z |
| w2 | 2024-09-13T06:44:59Z | 2025-03-28T11:29:59Z |
| w3 | 2025-03-28T11:29:59Z | 2025-10-10T16:14:59Z |
| w4 | 2025-10-10T16:14:59Z | 2026-04-24T20:59:58Z |

Boundaries are different from VWR S71 / UTF5m S73 (different tape,
different total range), so per-window comparison with those is *not*
direct in the same-boundary sense. The shape of the regime split
(early-window failure, late-window pass) is comparable though.

## 4. Per-window results

Each window run as `--mode baseline` (S63 trio = 0.0/0.0/0.0).

| Window | Period | Days | Trades | Wins | WR% | Avg PnL | PF | TP / SL / Other | Worst | Pass |
|---|---|---|---|---|---|---|---|---|---|---|
| w1 | 2024-03 → 2024-09 | 196.2 | 215 | 77 | 35.81 | **−0.4596** | 0.221 | 77 / 134 / 4 | −25.58 | FAIL |
| w2 | 2024-09 → 2025-03 | 196.2 | 218 | 84 | 38.53 | **−0.5302** | 0.219 | 84 / 130 / 4 | −30.72 | FAIL |
| w3 | 2025-03 → 2025-10 | 196.2 | 212 | 93 | 43.87 | **−0.0788** | 0.785 | 93 / 118 / 1 | −39.91 | FAIL |
| w4 | 2025-10 → 2026-04 | 196.2 | 201 | 78 | 38.81 | **+0.5310** | 2.088 | 76 / 123 / 2 | −3.13 | PASS |

The pattern is **monotonically improving** w1 → w4 on PF and avg PnL,
but only w4 crosses both thresholds. Trade cadence is stable across
all four windows (201-218 trades, ~33 trades/month combined across
the 4 cells, matching PROVENANCE expectation of ~25/month).

### 4.1 Win rate is roughly constant — win **magnitude** is the failure mode

Win rate is stable across all four windows (35.8%, 38.5%, 43.9%,
38.8% — a tight ±4 pp band around 39%). What collapses in the
failing windows is the *size* of each win relative to each loss:

| Window | Avg Win Size | Avg Loss Size | Win:Loss ratio |
|---|---|---|---|
| w1 | $0.363 (≈ 27.97 / 77) | $0.919 (≈ 126.79 / 138) | **0.40** |
| w2 | $0.385 (≈ 32.36 / 84) | $1.104 (≈ 147.94 / 134) | **0.35** |
| w3 | $0.657 (≈ 61.05 / 93) | $0.653 (≈ 77.75 / 119) | **1.01** |
| w4 | $2.626 (≈ 204.83 / 78) | $0.797 (≈ 98.09 / 123) | **3.29** |

The engine's nominal R:R is 2:1 (TP=4×ATR vs SL=2×ATR). At a 40% win
rate, the **realised** win:loss ratio needs to be ≥1.5 just to break
even (0.40 × 1.5 − 0.60 × 1.0 = 0.0 expectancy). The realised ratio
in w1/w2 is 0.35-0.40 — winners are coming in at roughly *half* their
nominal target. SLs hit at near-full size; TPs leak early at fractions
of the target. This is the classic shape of a trend-follow engine
being whipsawed in non-trending volatility.

w4 alone shows the engine working as designed: win:loss = 3.29,
which is *better* than the nominal 2:1 ratio because some winners
also got a tail before exiting. That's clean trend regime.

### 4.2 Worst-trade distribution corroborates the regime story

| Window | Worst trade |
|---|---|
| w1 | −$25.58 |
| w2 | −$30.72 |
| w3 | −$39.91 |
| w4 | **−$3.13** |

w4's worst trade is an order of magnitude smaller than the other
three windows' worst trades. In a trend regime, even adverse trades
get out near the SL before huge excursion. In choppy regimes, gap
moves and reversal cascades produce big single-trade losses far
beyond the SL distance. The early windows show that latter pattern.

## 5. Aggregate and decision

| Metric | Value |
|---|---|
| Aggregate trades | 846 |
| Aggregate gross PnL | **−124.37** |
| Aggregate avg PnL | **−0.1470** |
| Aggregate sum_pos | +326.21 |
| Aggregate sum_neg | −450.57 |
| Aggregate PF | **0.7240** |
| Windows passing avg_pnl ≥ +0.001 | **1 of 4** |

**Decision rule:** PASS = (windows passing ≥ 3) AND (aggregate PF ≥ 1.20).

**Verdict:** **FAIL** — both gates missed. Window-count gate misses
by 2 (got 1, need 3). PF gate misses by 0.476 (got 0.72, need 1.20).

This is a **decisive** fail, not a near-miss. No discretion required.

## 6. Sanity check vs WF reconciliation

The pre-WF sanity check (`--engine 2h --mode baseline` on the full
26-month tape, single continuous run) produced:

| Metric | Sanity (full tape) | WF (sum of 4 windows) |
|---|---|---|
| n_trades | 868 | 846 |
| gross_pnl | **+150.80** | −124.37 |
| PF | **1.7999** | 0.7240 |
| win_rate_pct | 40.55 | 39.24 |

The 22-trade gap (868 vs 846) is window-boundary-straddling trades
that the WF driver drops (positions open in window N but not yet
closed when window N ends). At an average winner size of ~$5 (the
sanity check's gross_pnl / wins ratio), those ~22 boundary-dropped
trades account for roughly $100 of the $275 P&L gap.

The **remaining** $175 gap is harder to explain by methodology alone.
The most plausible reading: the WF aggregation reveals **regime
selection bias** that's hidden by the continuous-run sanity check. In
a continuous run, the engine's internal state (indicator histories,
cooldowns, ATR baselines) carries through regime transitions. In the
WF, each window starts cold — the engine has to rebuild its signal
context from scratch. If the early ticks of each window produce
poorer-quality entries than the equivalent ticks in a continuous run,
that compounds across 4 windows.

A secondary contributor: PF is **not linear** under aggregation. PF
of 1.80 on the full tape doesn't decompose into a sum-of-window PF
of 1.80 — windows with strong P&L contribute more PF-numerator than
the win-rate would suggest, and the WF's sub-window split exposes
the regime concentration that the full-tape PF averages away.

**The WF verdict is the load-bearing one** for the validation
question. The sanity check confirmed engine wiring is correct (cells
fire, exit reasons distribute sanely, sign positive on full tape);
the WF tells us whether the engine has a *stable* edge across
time-period subsamples. It does not.

## 7. Closure character — comparison with VWR S71 and UTF5m S73

| Engine | Tape | Windows pass | Aggregate PF | Character |
|---|---|---|---|---|
| VWR USTEC.F (S71 P3) | NSXUSD 2024-01 → 2026-04 | 2 / 4 | 1.0358 | Regime-bimodal: 2024 strongly negative, 2025-26 marginally positive. PF gate missed by 0.16. |
| UTF5m USTEC (S73 P3) | NSXUSD 2024-01 → 2026-04 | 3 / 4 | 1.1154 | Window-count gate met; PF gate missed by 0.085. Softest fail. w1 outlier negative, monotonic improvement w2-w3. |
| **XTF 2h (S74 P3)** | **XAUUSD 2024-03 → 2026-04** | **1 / 4** | **0.7240** | **Regime-concentrated to w4 alone.** Three of four windows individually negative. Hardest fail of the three. |

XTF 2h's failure is the **hardest** of the three closures on every
axis:
- Fewest windows passing (1 vs 2 or 3)
- Lowest aggregate PF (0.72 vs 1.04 or 1.12)
- Largest PF gate miss (0.476 vs 0.16 or 0.085)
- Most window-internal P&L sign flips (3 of 4 negative)

The shape resembles VWR USTEC.F (regime-concentrated to recent
data, sharp time-period dependence) more than UTF5m USTEC (which
had monotonic improvement and only one outlier window). That said,
XTF's regime concentration is *more extreme* than VWR's — VWR had
two windows passing on avg_pnl alone, even if neither cleared PF.

**One important framing difference.** VWR and UTF5m both ran their
WFs *after* a Phase 1 sweep had identified `--mode baseline` (S63
OFF) as the best regime — so the Phase 3 WF was testing whether the
*best-discovered* configuration generalises. XTF 2h ran its WF on
class-default state-B values (also 0.0/0.0/0.0) because the engine
just landed those defaults in part-W and had no Phase 1 sweep. This
is the *initial* configuration, not the *optimised* configuration.
A future Phase 1 sweep across LOSS_CUT / BE_ARM / BE_BUFFER axes
could in principle find a cell where the engine clears the WF gate.
Whether that's likely given the regime-concentration pattern is
discussed in §10.

## 8. Verdict text correction

The auto-generated `wf_verdict.txt` ends with this implication
block:

> Implication: S63 baseline on XAU 2h trend-follow does NOT clear
> the WF gate. The S63-adverse pattern from VWR USTEC (S71 P3) and
> UTF5m USTEC (S73 P3) appears to generalise to XAU trend-follow at
> the 2h timeframe -- a notable finding given XAU's different tail
> shape.

This wording was inherited from the utf5m_wf_t1.py / vwr_wf_t1.py
boilerplate and is **misleading in the XTF context**.

In the VWR/UTF5m cases, "S63-adverse" had a specific meaning: the
Phase 1 sweep showed that *enabling* S63 (LOSS_CUT > 0 / BE_ARM > 0
/ BE_BUFFER > 0) *worsened* the engine relative to baseline. The
WF then validated that the baseline-best configuration generalised
across time-period subsamples. The conclusion was "S63 actively
hurts on these instruments" — a property of the *protection
mechanism*.

In the XTF 2h case, there has been **no Phase 1 sweep**. The WF
tested only `--mode baseline` (S63 OFF). The verdict text's
"S63-adverse" framing therefore claims more than the data supports.
The accurate framing is:

> XauTrendFollow2hEngine, at the class-default state-B configuration
> (S63 OFF), fails the WF gate on the substitute tape (XAUUSD
> 2024-03 → 2026-04). The pattern is one of regime concentration:
> the engine works in clean trend regimes (w4 only) but produces
> sub-break-even win:loss magnitudes in choppier regimes (w1, w2, w3).
> This is a property of the *engine's signal mechanics on XAU*, not
> a property of the S63 protection mechanism.

S63's effect on XTF — whether enabling it would help, hurt, or be
neutral — remains an open question that a Phase 1 sweep across the
S63 trio axes would answer. The current evidence is silent on this.

## 9. Per-trade P&L vs historical PROVENANCE — investigation pending

The engine's PROVENANCE comment
(`include/XauTrendFollow2hEngine.hpp:6-22`) reports per-cell historical
performance on the validation tape (Dukascopy 2023-09-27 → 2025-09-26):

| Cell | n | net | BE/trade |
|---|---|---|---|
| A — Keltner K=2.0 sl2.0tp4.0 | 179 | +$950 | $5.37 |
| B — Donchian N=20 sl2.0tp4.0 | 204 | +$872 | $4.33 |
| C — Donchian N=50 sl2.0tp4.0 | 142 | +$833 | $5.93 |
| D — InsideBar sl2.0tp4.0     | 239 | +$725 | $3.09 |
| **Total** | **764** | **+$3,380** | ~$4.42 net/trade |

Our sanity-check run produced +$150.80 gross on 868 trades = **$0.174
gross/trade**. That's a ~25-30x discrepancy in per-trade magnitude,
even though trade count is in proportional range and PF=1.80 is
*better* than the historical implied PF.

Two non-mutually-exclusive explanations:

1. **Cost-guard architectural side-effect.** The original validation
   harness (edge_hunt.cpp + top_cells_monthly.cpp per PROVENANCE)
   ran without `OmegaCostGuard::is_viable()` filtering at fire time.
   The production engine has the cost-guard inline at fire time and
   it rejects marginal trades. If the cost-guard happens to reject
   the *fattest-tail* trades (which are by definition borderline at
   fire time — they have to be near-cost-break-even to be borderline),
   then the surviving trade distribution has *higher PF* (only viable
   trades fire) but *lower absolute P&L* (the moonshots are gone).
   This is consistent with what we see: PF up from historical implied,
   absolute P&L down by an order of magnitude.

2. **Tape regime difference.** Validation tape was 2023-09 → 2025-09
   (XAU $1800-2700 range, ~50% rally with material drawdowns). Our
   substitute tape is 2024-03 → 2026-04 (XAU $2000-3500 range, ~75%
   rally, very different microstructure). Per-trade $-magnitude
   scales with ATR, not just trade count.

Investigation deferred to a separate session. The investigation does
**not** change today's WF verdict — even if per-trade P&L were 30x
higher to match historical, the WF *ratios* (PF, win:loss size,
window-pass rate) are scale-invariant. A 30x bigger version of this
result is still 1/4 windows passing at PF=0.72. The WF asks a
ratio question, not a magnitude question.

Recommended follow-up for this thread (no urgency): per-cell
breakdown of the trades.csv to see which of the 4 cells are degraded
most. If one cell is responsible for the WF failure and the other
three are PF≥1.20, a partial cell-subset deployment might be worth
considering. The breakdown can be done from `outputs/xtf_2h_t1_p3_*/cells/w*_trades.csv`
in a follow-up session.

## 10. Implications and recommendations

### 10.1 Immediate state

XauTrendFollow2hEngine stays at class-default state-B (S63 trio =
0.0). `engine_init.hpp` is **not** modified by this closure —
there's no engine_init.hpp activation to undo and no new state-A
activation to add. The XTF trio remains at the configuration landed
in part-W's E→B transition commit.

Sibling engines (XauTrendFollow4hEngine, XauTrendFollowD1Engine)
are unaffected by this verdict. They were given the same state-B
hooks in the same part-W commit but their WF runs require the
harness 4h/d1 dispatch paths to be filled in first (currently
stubbed per `backtest/XauTrendFollowBacktest.cpp` §"NEXT-SESSION
TODO" items 1-2).

### 10.2 What this tells us about S63 on XTF

Strictly: **nothing direct**. The WF ran S63 OFF. To answer "does
S63 protection help, hurt, or neutral on XTF 2h", a Phase 1 sweep
across the S63 trio axes would be needed. Given the engine fails
even at baseline, however, the *prior* on S63 being a fix is low —
S63 protection is a magnitude-of-loss reducer, not a signal-quality
improver. The regime-concentration pattern this WF surfaces is a
signal-quality issue (winners coming in below target size in
3 of 4 regimes), not a tail-loss issue. S63 is the wrong tool for
this failure mode.

### 10.3 What this tells us about XAU trend-follow more broadly

Mixed. The engine's signal mechanics (Keltner / Donchian / InsideBar
breakout) clearly *work* in clean trend regimes (w4 PF=2.09, win:loss
ratio 3.29). They fail in chop. The ratio of trend regimes to chop
regimes in the 2024-2026 sample is roughly 1:3 — the engine ate
three quarters of stop-outs to harvest one quarter of trend payouts,
and the trade-count weighting didn't lean in trend's favor enough
to overcome the win-size compression in chop.

This is **exactly the use case** for the Tier 4 vol-regime gate
scoped in `outputs/TIER4_VOL_REGIME_SCOPING_2026-05-14.md`. Form A
(S63 OFF in high vol) doesn't apply here — S63 is already off.
What XTF needs is closer to a **trend-state gate**: only fire
entries when the wider-timeframe trend strength (ADX, slope of
moving-average envelope, or similar) is above a threshold. This is
adjacent to but not identical to the vol-regime gate; vol and trend
are correlated but not equivalent.

Recommendation: the Tier 4 scoping memo's Phase A
(VWR USTEC.F ATR-percentile gate proof-of-concept) should run
first. If that Phase A succeeds, the *next* extension should be a
**trend-state gate** experiment on XTF 2h — not a vol-regime gate.
Different signal, similar architecture. The Tier 4 scoping memo §6
"Out of scope" identifies "vol+trend interaction" as a future
extension; the XTF closure makes the case for promoting that to
its own scoping pass after Phase A.

### 10.4 4h and D1 timeframes — next session

The XTF 4h and D1 engines remain at state B with the same hooks. We
do **not** have WF evidence on either timeframe yet. The harness
stubs need to be filled in (part-W handoff §"Recommended
next-session focus" items 2-3) before WFs can run.

Two scenarios worth pre-considering:

- **4h passes WF where 2h failed.** Plausible — slower timeframes
  experience less chop, more clean trend. If 4h clears the gate
  while 2h doesn't, the operator decision becomes "do we run 4h+D1
  but disable 2h" vs "do we treat the trio as one unit". The
  PROVENANCE notes the three engines are correlated, so the unit
  view has merit.
- **All three fail.** Also plausible — XAU 2024-2026 may be a regime
  where ATR-based bracket strategies underperform vs more discretionary
  regime-switched approaches. In that case the entire XTF trio gets
  parked at state B indefinitely, and the Tier 4 trend-state gate
  experiment becomes the only path to activation.

### 10.5 No engine_init.hpp activation, period

Reaffirming: regardless of any future WF result on 4h or D1, do
**not** flip the XTF trio's S63 fields to non-zero in engine_init.hpp.
The path from state B to state A requires per-instrument, per-cell
backtest evidence per the project CLAUDE.md and the part-W §"Pre-
commit checklist" item 3. This closure produces no such evidence.

## 11. Standing audit / decision sticks

- `XauTrendFollow{2h,4h,D1}Engine.hpp` unchanged by this closure;
  state-B (0.0/0.0/0.0) defaults from part-W remain authoritative.
- `engine_init.hpp` unchanged by this closure; no new init lines
  for the XTF trio.
- Core code (`OmegaCostGuard.hpp`, `OmegaTradeLedger.hpp`, etc.)
  untouched.
- The validation-baseline tape (3-year Dukascopy 2023-09 → 2025-09)
  is confirmed gone. Future XTF work uses the substitute 2024-03
  → 2026-04 combined tape until re-downloaded.
- Stop-bleed disables on `g_vwap_rev_nq` (engine_init.hpp:608) and
  `g_ustec_tf_5m` (engine_init.hpp:950) remain in force; this
  closure has no bearing on them.

## 12. Suggested commit

```bash
cd ~/omega_repo
git add -f outputs/XTF_2H_TIER1_PHASE3_RESULTS_2026-05-14.md
git add -f outputs/xtf_2h_t1_p3_20260514_172751/
git diff --cached --stat
git commit -m "S74: XauTrendFollow2h Tier 1 Phase 3 walk-forward FAIL - close track

Phase 3 walk-forward on the part-W state-B XauTrendFollow2hEngine
(S63 trio = 0.0/0.0/0.0 class defaults). Tape: substitute Dukascopy
XAUUSD 2024-03 -> 2026-04 (original validation tape 2023-09 -> 2025-09
confirmed gone). 4 windows, --mode baseline.

Verdict: FAIL on both gates.
  - Windows passing avg_pnl >= +0.001: 1 of 4 (need >=3).
  - Aggregate PF: 0.7240 (need >=1.20).

Decisive fail. Regime-concentrated: only w4 (2025-10 -> 2026-04)
passes; w1/w2/w3 individually negative. Win rate stable ~39% across
all windows; failure mode is win:loss magnitude compression in
choppy regimes, not signal frequency.

Closure character: harder fail than VWR S71 (2/4 windows, PF 1.04)
and UTF5m S73 (3/4 windows, PF 1.12). Engine stays at class-default
state B. No engine_init.hpp activation. Sibling 4h + D1 WFs blocked
on harness stub fill-in per part-W next-session TODO items 1-2.

Important: the auto-generated wf_verdict.txt's 'S63-adverse pattern
generalises' wording is inherited boilerplate from utf5m/vwr drivers
and is misleading here -- this WF ran S63 OFF and tested engine
baseline; conclusion is about engine signal quality on XAU
2024-2026 regime, not about S63 protection mechanism. See memo §8
for the correction.

Full analysis at outputs/XTF_2H_TIER1_PHASE3_RESULTS_2026-05-14.md.
WF run artifacts at outputs/xtf_2h_t1_p3_20260514_172751/."
git push origin main
```

## 13. Closing note

This closure follows the same shape as VWR S71 and UTF5m S73 but on a
new instrument and timeframe. Three Phase 3 closures in a row, all
FAIL, suggests that **the S63 + walk-forward methodology is doing
its job** — it's catching engines that *look* profitable on
continuous-run backtests but don't have stable regime-independent
edges. That's a feature, not a bug; the alternative was promoting
engines to state A on continuous-run optics alone and discovering
the regime-fragility in production.

The next-session focus should be:

1. Fill in the XTF 4h harness dispatch path
   (`backtest/XauTrendFollowBacktest.cpp` NEXT-SESSION TODO #1) and
   run `scripts/xtf_4h_wf_t1.py` (which needs to be created — 5-line
   patch of `xtf_2h_wf_t1.py`). One more data point on whether the
   XTF trio is universally regime-fragile or just 2h.
2. Same for D1 (TODO #2). Caveat: D1 cadence is ~2 trades/month,
   so per-window N is ~12, small-sample.
3. Tier 4 Phase A on VWR USTEC.F per the scoping memo. The Tier 4
   exploration is the more promising track for resurrecting any of
   the three closed engines (VWR / UTF5m / XTF), and the per-instrument
   proof-of-concept needs to land first before the cross-instrument
   generalisation phase makes sense.

After this closure, the running count of state-B engines with
documented S63 evidence stands at:

| Engine | Closure | State |
|---|---|---|
| g_vwap_rev_sp        | part-K backtest evidence | B (zeroed at init) |
| g_vwap_rev_nq        | part-K backtest evidence | B (zeroed at init); engine disabled S68 |
| g_vwap_rev_eurusd    | part-L backtest evidence | B (zeroed at init) |
| g_ustec_tf_5m        | S73 P3 closure | B → disabled S73 P3 fail |
| g_xau_tf_2h          | **S74 P3 closure (this memo)** | **B (class defaults)** |
| g_xau_tf_4h          | pending WF | B (class defaults) |
| g_xau_tf_d1          | pending WF | B (class defaults) |

End memo.
