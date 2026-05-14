# Tier 4 vol-regime gate — scoping memo — 2026-05-14 (part W)

**Status:** SCOPING (focused depth). Not implementation. Multi-session work
ahead; this memo defines the design question, decision criteria, and
implementation phasing. Read alongside the VWR structural rework scoping
memo (`outputs/VWR_USTEC_STRUCTURAL_REWORK_SCOPING_2026-05-14f.md`)
which establishes structural patterns reused here.

## 1. Motivation

Two independent Phase 3 walk-forward failures during the 2026-05-14 session
arc converged on the same recommendation: a vol-regime gate that conditions
S63 in-flight protection on observed-volatility state at entry time.

**Failure 1 — VWR USTEC.F (S71 P3):**
The VWAPReversion engine on USTEC.F failed the Phase 3 WF gate. Aggregate
PF was below 1.20, with windows showing high variance dominated by 1-2
catastrophic loss clusters per window. The S63 protection was empirically
adverse: each cell with S63 ON had worse expectancy than the baseline (S63
OFF). Phase 3 closure memo §6.2 recommended exploring a Tier 4 gate that
disables S63 during high-volatility regimes where the cold-loss cut
prematurely terminates trades that would have recovered.

**Failure 2 — UTF5m USTEC.F (S73 P3):**
The UstecTrendFollow5m engine on USTEC.F failed the same Phase 3 gate
under the same protocol. Phase 1 (S72) had already shown S63 was
adverse; Phase 3 confirmed the baseline itself did not clear the gate
(PF 1.1154 vs 1.20). Phase 3 closure memo §6.3 independently recommended
a Tier 4 vol-regime gate, observing that 3 of 4 WF windows passed and
the failure was localised to a single 2024-H2 window with anomalously
high realised volatility.

**Pattern.** Both failures share:
- USTEC.F instrument (range-expansion-heavy index).
- S63 protection adverse OR baseline insufficient on a single high-vol
  sub-window.
- Decision-rule-correct rejection: the pre-committed pass criterion
  fired and the engines stayed disabled.

The recommendation in both Phase 3 memos is identical: a vol-regime gate.
This memo scopes that gate as a single design pattern that could be
applied to both engines (and others showing similar behavior).

## 2. The design question

**Can a regime-conditional S63 outperform an always-on or always-off S63
on USTEC.F, on the same WF protocol?**

Two parametric forms to evaluate:

**Form A — S63 OFF in high vol, ON in low vol:**
The intuition is that S63 cuts cold-loss positions early; during high vol,
this premature termination loses an otherwise-recoverable trade. During
low vol, the absence of S63 means a clean adverse trade rides to the
ATR-based SL and takes a larger loss than necessary. So the gate routes
S63 to the regime where it adds value.

**Form B — S63 ON in high vol, OFF in low vol:**
Inverse intuition. Worth testing as a null hypothesis. The Phase 3
evidence does NOT immediately discriminate between Form A and Form B
because we don't have per-window-vol-binned trade analysis.

**Form C — vol-conditional parameter scaling:**
S63 always on, but `LOSS_CUT_PCT` and `BE_BUFFER_PCT` scale with realised
vol. Higher vol → wider cuts. Continuous parameter response rather than
binary gate. More flexible but harder to backtest cleanly.

The scoping recommendation is to test all three (A, B, C) against the
same WF protocol that VWR S71 P3 and UTF5m S73 P3 use, and pick the
form that produces the best aggregate PF with the most stable per-window
distribution.

## 3. Vol-regime signal — design choices

Three signal candidates, in increasing complexity:

### 3.1 ATR-percentile gate (simplest)

At entry time, compute the current ATR(14) value and rank it within a
rolling lookback window (e.g., 30 days). If the ATR percentile is
above a threshold (e.g., >75th percentile), classify as "high vol".

**Pros:** existing ATR computation; minimal new state; binary decision.

**Cons:** ATR is lagging; smoothing decay means recent vol spikes are
underweighted; percentile cutoff is a hyperparameter that needs its own
sweep.

### 3.2 GARCH-style realised-vol gate (mid complexity)

Track rolling realised volatility from tick-to-tick log-returns with a
mean-reverting AR(1) state estimator. Threshold = realised-vol mean +
1.5 × stdev (or another z-score).

**Pros:** standard quant pattern; explicit mean-reversion; well-known
behavior.

**Cons:** new state (~50 lines of code); requires per-engine tuning of
half-life parameter; lookback period selection is its own sweep
dimension.

### 3.3 HMM regime-state gate (highest complexity)

The project already has an HMM regime classifier (per the `regime`
field on `TradeRecord` and the `HmmRegime` enum in `globals.hpp`).
Conditioning S63 on the existing regime classification reuses
infrastructure but couples S63 behavior to the HMM's regime
definition.

**Pros:** existing infrastructure; consistent with how other engines
condition behavior on regime; no new state machine.

**Cons:** HMM classifier may not capture *short-term* vol shifts that
matter for S63 timing; coupling to a complex multi-state classifier
makes per-regime parameter tuning combinatorial.

### 3.4 Recommendation

Start with **3.1 ATR-percentile gate** because:
- Fastest to implement (~20 lines per engine).
- Easiest to validate (single threshold parameter).
- Cleanest to attribute results to (no confounding from a more complex
  vol estimator).
- If 3.1 passes, the team has evidence that "regime-conditional S63"
  is a viable pattern. Then escalate to 3.2 or 3.3 if 3.1 cell
  optimum is unstable across instruments.

## 4. Phasing plan

This is multi-session work. Suggested phasing:

### Phase A — VWR USTEC.F single-instrument proof (1 session)

- Add ATR-percentile gate to VWAPReversionEngine.
- Sweep `vol_pctile_threshold` in {50, 60, 70, 75, 80, 85, 90}.
- For each threshold, run baseline + Form A + Form B WF (same 4-window
  cuts as S71 P3).
- Output: per-threshold WF summary CSVs + a verdict memo identifying
  the best-performing form/threshold combination on USTEC.F VWR.

**Decision criterion:** Phase A passes if the best cell beats baseline
(no-vol-gate) by ≥10% on aggregate PF AND clears the standard ≥1.20 PF
+ ≥3/4 windows pass gate. If not, Tier 4 is killed on USTEC.F VWR.

### Phase B — UTF5m USTEC.F replication (1 session)

If Phase A produces a winning form/threshold on VWR, replicate the
test on UTF5m USTEC.F with the same form and threshold range. The
prior is that the same gate that helps VWR may help UTF5m given the
USTEC.F instrument-shared characteristic.

**Decision criterion:** Phase B passes if the gate generalises (best
threshold within ±1 cell of the VWR optimum AND clears the gate).
If not, the form is instrument-strategy-specific, not USTEC-specific.

### Phase C — XAU trend-follow generalisation (1-2 sessions)

If Phase A AND Phase B both pass, run the same gate on the XAU
TrendFollow trio (2h/4h/D1) using the XauTrendFollowBacktest harness
landed in part W. This tests whether the pattern generalises beyond
USTEC.F to a different instrument family.

If Phase A passes but Phase B fails, skip Phase C — the gate is too
strategy-specific to generalise.

### Phase D — Production-side wiring (1 session)

If Phases A-C produce a coherent recommended form/threshold, design
the production-side wiring:
- Where the vol-percentile signal is computed (engine vs shared
  infrastructure).
- How it's exposed for testing (CLI override vs runtime config).
- Documentation of the per-instrument tuning ranges in the engine
  headers.

This phase is a "tooling" session — no new strategy evidence, just
clean integration with the existing engine_init.hpp idiom.

## 5. Open design questions

Items the operator should answer (or note for the team) before Phase A
starts:

1. **Lookback period for ATR percentile.** 30 days is the obvious
   starting point; 60 days is more stable but lags more. The sweep
   should include both extremes plus one middle value.

2. **Percentile cutoff binary vs interpolated.** A hard threshold at
   75th percentile is binary; an interpolated cutoff (S63 PCT scales
   linearly between 50th and 75th) is continuous. Continuous is
   harder to reason about but may produce smoother results.

3. **Per-cell vs per-engine gate.** In the multi-cell engines
   (XauTrendFollow x4-6 cells), should the vol gate apply globally to
   all cells of an engine, or per-cell? Per-cell is more flexible but
   also more degrees of freedom per backtest.

4. **Symmetric vs asymmetric.** Does the vol gate apply to both phases
   of S63 (BE_RATCHET + LOSS_CUT) uniformly, or are they conditioned
   independently? VWR S71 P3 §6.4 hinted that BE_RATCHET was the more
   adverse phase; LOSS_CUT may be net-neutral. If so, an asymmetric
   gate (vol-conditional BE_RATCHET, always-on LOSS_CUT) might be
   the actual optimum.

5. **Validation lookback.** The standard WF protocol uses ~30 months
   of XAU data and ~404 days of USTEC data. Phase A should commit to
   the same windows so results are comparable to S71 / S73.

## 6. Out-of-scope (deferred to its own scoping pass)

- Cross-instrument vol regime contagion (does GER40 vol predict USTEC
  S63 effectiveness?).
- Intraday vol gate (session-of-day-based rather than ATR-based).
- L2-microstructure-informed vol gate (XAU L2 corpus only).
- Vol-regime + trend-strength interaction (combined gate).

All are valid extensions but each multiplies the design space. Lock
in the ATR-percentile gate first; revisit if it fails or if a Phase
B+C result suggests the simpler form is insufficient.

## 7. Pre-implementation checklist

Before any code lands on Phase A:

1. The operator agrees this is worth the multi-session investment.
2. Decide on **Form A vs Form B vs Form C** as primary test (Form A is
   the recommendation; Form B as null hypothesis ride-along).
3. Decide on **ATR-percentile signal** as the gate (other options are
   §3.2 and §3.3; the recommendation is §3.1).
4. Confirm the WF protocol (4-window, avg_pnl ≥ +0.001, PF ≥ 1.20)
   is unchanged — the gate is judged on the same criterion as S63
   itself.

If all four lock in, Phase A can start with engine code changes to
`VWAPReversionEngine` adding the gate + a new sweep driver
`scripts/vrev_vol_gate_p1.py` adapted from the existing
`scripts/vrev_wf_t1.py`.

## 8. Expected outcomes

Three plausible end-states:

**Outcome 1 (~40% likelihood) — Tier 4 gate clears the WF gate.**
A specific ATR-percentile threshold (probably 70-80th) restores VWR
USTEC.F to passable expectancy. UTF5m USTEC.F replicates. XAU
generalisation is partial. The team has a new tool for the toolbox.

**Outcome 2 (~40% likelihood) — Gate marginally helps but doesn't clear.**
Best Tier 4 cell beats baseline by 5-10% on aggregate PF but stays
below the 1.20 threshold. Phase 1 closure memo captures the
near-miss; engines stay disabled; the team has evidence that the
adversity isn't reducible to a simple vol-percentile signal — points
toward signal-side rework (per VWR P3 §6.1).

**Outcome 3 (~20% likelihood) — Gate doesn't help.**
No threshold clears or even meaningfully improves. Killswitch the
Tier 4 line of investigation; pivot to the signal-side rework or
accept that USTEC.F is structurally not suited to these strategies.

Plan for outcome 2 as the likely case; outcome 1 is the upside;
outcome 3 is the safety pivot.

## 9. Reference table — what this memo replaces

| Source | What it asks for |
|---|---|
| VWR P3 §6.2 (outputs/VWR_USTEC_TIER1_PHASE3_RESULTS_2026-05-14.md) | "Explore a Tier 4 vol-regime gate that disables S63 during high-vol regimes" |
| UTF5m P3 §6.3 (outputs/UTF5M_PHASE3_RESULTS_2026-05-14.md) | "Tier 4 vol-regime gate — leading candidate per Phase 3 closure" |
| Part-V handoff §"Recommended next-session focus" item 3 | "Tier 4 vol-regime scoping memo (3+ hours, multi-session)" |
| Part-W handoff (this session) | "Tier 4 scoping memo done; Phase A implementation queued" |

This memo locks in the scoping inputs from the three Phase 3 closure
memos that motivated it. Phase A implementation work starts from §4
above.

## 10. Closing note

The Tier 4 line of investigation is **research**, not maintenance.
Its outcome is genuinely uncertain. Pre-commit to:

1. **Treating outcome 2 (marginal help) as a FAIL**, not a near-pass.
   The 1.20 PF threshold is the gate. Anything below is "not yet";
   don't flip engines on based on near-misses.

2. **Killing the line cleanly if outcome 3** rather than incremental
   "let me try one more threshold". The Phase 3 protocol is decisive
   for a reason.

3. **Documenting the negative result thoroughly if outcome 2 or 3.**
   A well-documented "vol regime gate empirically does not rescue
   USTEC.F S63 protection" is a valuable artifact even if the engines
   stay disabled — it closes a hypothesis cleanly so future sessions
   don't redo the work.

The same discipline that made VWR S71 P3 and UTF5m S73 P3 produce
crisp negative results applies to Tier 4. The decision rule pre-commits
the verdict; the work is the evidence, not the persuasion.
