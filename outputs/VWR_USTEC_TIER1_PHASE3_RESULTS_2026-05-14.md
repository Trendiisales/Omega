# VWR USTEC.F — Tier 1 Phase 3 walk-forward validation — 2026-05-14

**Status:** COMPLETE. 4-window WF on (open=12, close=21). Verdict:
**FAIL.** 2 of 4 windows passed `avg_pnl ≥ +0.001` (threshold ≥3),
aggregate PF=1.0358 (threshold ≥1.20). The Phase 2 edge is
**regime-concentrated** — entirely driven by 2025-2026; 2024 is
solidly negative. Closing the VWR USTEC.F retune track. Engine
stays disabled at `engine_init.hpp:608`.

## 1. Scope

Phase 3 validated the Phase 2 carry-forward `(open=12, close=21)`
under a 4-window walk-forward split. Per scoping memo
`outputs/VWR_USTEC_STRUCTURAL_REWORK_SCOPING_2026-05-14f.md` §5
and Phase 2 results memo §5.

Tape split into 4 non-overlapping equal-duration windows on a single
streaming pass. Each window run through the harness with the Phase 2
carry-forward session settings, in `--mode baseline` (S63 trio
zeroed, matching Phase 1/2 protocol).

Pass criterion: ≥3 of 4 windows produce `avg_pnl ≥ +0.001` AND
aggregate OOS profit factor ≥ 1.20.

The "cost-pessimistic PF" framing uses the harness's per-trade
reported `gross_pnl` directly; `ExecutionCostGuard::is_viable()` has
already filtered trades that cannot cover costs at TP, so the
reported PnL reflects the round-trip economics of trades the engine
actually fired.

## 2. Run

```bash
scripts/vrev_wf_t1.py /Users/jo/Tick/NSXUSD_merged.csv \
    --output-dir outputs/vrev_t1_p3_run/
```

Wall-clock: ~95 seconds total (single streaming pass + 4 sequential
harness runs at 11-30s each).

Preflight: tape 4.43 GB, peak temp 1.11 GB (largest window),
free disk 2.84 GB. Mac was at 100% disk utilisation — see §6
operational note.

The original `vrev_wf_t1.py` v1 ran out of disk because it held all 4
splits open concurrently (peak temp ≈ 4.4 GB). Rewritten this session
to stream one window at a time, run the harness, and delete the
split before producing the next window. Peak temp drops to ~1/N of
the tape.

## 3. Tape coverage

Auto-detected by the driver from the first/last data lines:

| Field | Value |
|---|---|
| Format detected | A_TBA |
| start_iso (UTC) | 2024-01-01T23:00:00Z |
| end_iso (UTC) | 2026-04-10T21:14:59Z |
| total days | 829.93 |
| window duration | 207.48 days each |

| Window | Start (UTC) | End (UTC) |
|---|---|---|
| w0 | 2024-01-01T23:00:00Z | 2024-07-27T10:33:45Z |
| w1 | 2024-07-27T10:33:45Z | 2025-02-19T22:07:30Z |
| w2 | 2025-02-19T22:07:30Z | 2025-09-15T09:41:14Z |
| w3 | 2025-09-15T09:41:14Z | 2026-04-10T21:14:59Z |

## 4. Per-window results

Each window run as `--mode baseline --session-open-hour 12
--session-close-hour 21`.

| Window | Days | Trades | Avg PnL | PF | Pass |
|---|---|---|---|---|---|
| w0 (2024-H1) | 207.48 | 1010 | **−0.006530** | 0.932 | FAIL |
| w1 (2024-H2→2025-Q1) | 207.48 | 730 | **−0.002037** | 0.985 | FAIL |
| w2 (2025-Q1→Q3) | 207.48 | 1302 | +0.009651 | 1.067 | PASS |
| w3 (2025-Q4→2026-Q2) | 207.48 | 1238 | +0.012778 | 1.087 | PASS |

The pattern is sharply bimodal: 2024 strongly negative, 2025-2026
marginally positive. There is no chronological window with both a
positive `avg_pnl` AND a PF approaching the +1.20 robustness
threshold. The Phase 2 `+0.004876` full-tape expectancy was a
**weighted average** of these two regimes, not evidence of a stable
edge.

## 5. Aggregate and decision

| Metric | Value |
|---|---|
| Aggregate trades | 4280 |
| Aggregate gross PnL | +20.31 (≈ matches Phase 2 `+20.84`) |
| Aggregate avg PnL | +0.004746 |
| Aggregate PF | **1.0358** |
| Windows passing avg_pnl ≥ +0.001 | **2 of 4** |

**Decision rule:** PASS = (windows passing ≥ 3) AND (aggregate PF ≥ 1.20).

**Verdict:** **FAIL** on both criteria.

The "windows passing" threshold misses by 1 (2 of 4 vs ≥3 of 4) — that
alone could be argued as marginal. But the aggregate PF is far below
the robustness threshold: 1.0358 is essentially break-even, well below
the 1.20 the scoping memo set as the gate. The two passing windows have
PFs of 1.067 and 1.087 — even the "good" regime is weak.

## 6. Cross-window observations

### 6.1 Regime concentration is the dominant finding

The 2024 windows produce strongly negative `avg_pnl` (−0.00653 and
−0.00204); the 2025-2026 windows are positive (+0.00965 and +0.01278).
No partial overlap between the two regimes — the breakpoint is around
mid-2025.

This is a clean illustration of why walk-forward exists: an aggregate
+0.004876 result masks a bimodal distribution where one regime is the
edge and the other is a drag. Phase 2 saw only the average, and Phase 2
correctly recommended the WF gate (this isn't a Phase 2 error — it's
exactly the kind of finding WF was designed to surface).

### 6.2 w1 trade count is anomalously low

The four windows produce 1010, 730, 1302, 1238 trades respectively.
w1 (2024-H2 → 2025-Q1) has 28-44% fewer setups than the others. The
windows are equal-duration, so this is a real regime difference in
*how often the engine's entry conditions trigger*. Plausible
explanations:

- Lower realised volatility in 2024-H2 produces fewer
  EXTENSION_THRESH_PCT-qualifying setups (`abs_dev_pct >= 0.40%` in
  USTEC's case).
- Tighter intraday range constrains the EWM-VWAP deviation amplitude.
- Reduced overnight session leakage in the (12, 21) gate produces less
  qualifying material when the underlying isn't moving.

This vol-regime dependence is the most actionable finding for any
future signal-shape redesign — see §7.

### 6.3 Even the passing windows are weak

w2 PF=1.067 and w3 PF=1.087 are barely positive. A robust mean-reversion
strategy should produce PFs of 1.2+ in its good periods. That even the
positive regime tops out at 1.087 says the underlying edge is fragile,
not just time-localised. A different signal construction may be
necessary, not just a regime gate on top of the existing signal.

### 6.4 Reconciliation with Phase 2

Phase 2 reported: trades=4274, gross=+20.84, avg=+0.004876.
Phase 3 sum:     trades=4280, gross=+20.31, avg=+0.004746.

Match within rounding. The 6-trade and 0.53 PnL deltas are boundary
effects from how the WF allocates ticks at the window edges. The Phase
2 and Phase 3 setups are equivalent.

## 7. Outcome — Tier 4 (signal-shape redesign) recommended

Per scoping memo §4 reasoning: if Tier 1 fails its disqualifying test,
Tier 2/3 (data-pipeline rework) is skipped and the recommendation goes
to Tier 4 — redesigning the signal itself. Tier 1 has now failed at
the walk-forward stage; the entry-side parameter surface and the
session-window axis are exhausted.

**Closure call:** the VWR USTEC.F retune track is closed. The engine
stays disabled at `engine_init.hpp:608` (`g_vwap_rev_nq.enabled =
false`, S68 stop-bleed). No re-enable is recommended pending a Tier 4
signal redesign.

If a future session pursues Tier 4, the highest-priority redesign
candidates suggested by this Phase 3 data are:

1. **Daily volatility regime gate.** Only allow the engine to fire on
   days where prior 5-day realised volatility (or ATR) exceeds a
   threshold. The w1 anomaly suggests low-vol regimes are hostile
   to this signal even in its (12, 21) session window. A simple
   ATR ≥ X filter could lift w1 from negative to neutral by
   suppressing the marginal trades that bleed.

2. **Multi-timeframe VWAP anchoring.** Currently the engine maintains
   one EWM-VWAP with `HALF_LIFE = 7200s` (2hr). Phase 1's
   ewm-half-life-sec axis showed 7200s is the local optimum but
   nothing dramatically positive is available on that axis alone.
   A redesign could use weekly-anchored VWAP for extension cap +
   intraday VWAP for entry trigger.

3. **Direction-conditional entry filter.** The Phase 1 result that
   shrinking TP_FRACTION increases hit rate but worsens expectancy
   implies losers don't reverse far enough. A directional momentum
   filter (e.g. don't take longs when the trailing 1hr trend is
   strongly down) might lift the loser tail.

None of these are scoped here; they're the queue for a Tier 4
session if/when the operator decides VWR USTEC.F is worth revisiting.

The two follow-ups flagged at Phase 1 §6 (the `ext-sl-ratio`
plateau and the `tp_rate ≥ 5%` gate calibration) are now lower
priority since the engine is closed-out, but worth noting for any
future VWR work:
- The plateau likely indicates a downstream SL handler ignores
  `sig.sl`. Worth a `Grep` pass for `sig.sl` consumers when
  convenient.
- The `tp_rate ≥ 5%` gate proved unreachable across 60+ cells —
  any future VWR closure-rule iteration should drop it to ≥3%
  or remove it entirely.

## 8. Operational note — Mac disk

The Mac was at 100% disk utilisation (2.84 GB free of 460 GB) when
the WF ran. The original `vrev_wf_t1.py` failed with `[Errno 28]
No space left on device` because it held all 4 splits open
concurrently (~4.4 GB peak temp). The rewritten driver streams one
window at a time and caps peak at ~1.1 GB, which fit in the
available 2.84 GB.

This is fine for now but worth flagging — the disk is running close
to the bone. Future sweeps on this tape will work, but anything
larger or multi-tape will struggle. Recommend freeing some disk
when convenient.

## 9. Artifacts

- Summary CSV: `outputs/vrev_t1_p3_run/wf_summary.csv`
- Verdict text: `outputs/vrev_t1_p3_run/wf_verdict.txt`
- Per-window reports: `outputs/vrev_t1_p3_run/cells/w*_report.csv`
- Per-window trades: `outputs/vrev_t1_p3_run/cells/w*_trades.csv`
- Per-window stderr: `outputs/vrev_t1_p3_run/cells/w*_stderr.log`
- This memo: `outputs/VWR_USTEC_TIER1_PHASE3_RESULTS_2026-05-14.md`
- Driver: `scripts/vrev_wf_t1.py`

## 10. Commit suggestion

```bash
cd ~/omega_repo
git add -f outputs/VWR_USTEC_TIER1_PHASE3_RESULTS_2026-05-14.md
git add scripts/vrev_wf_t1.py
git diff --cached --stat
git commit -m "S71 Phase 3: VWR USTEC.F Tier 1 walk-forward FAIL - close track, recommend Tier 4

4-window walk-forward on Phase 2 carry-forward (open=12, close=21).
Verdict: FAIL.

Per-window results (avg_pnl / PF):
  w0 (2024-H1)            : -0.00653 / 0.932  FAIL
  w1 (2024-H2 -> 2025-Q1) : -0.00204 / 0.985  FAIL
  w2 (2025-Q1 -> Q3)      : +0.00965 / 1.067  PASS
  w3 (2025-Q4 -> 2026-Q2) : +0.01278 / 1.087  PASS

Aggregate: 4280 trades, avg=+0.004746, PF=1.0358.
2 of 4 windows pass (threshold >=3). Aggregate PF 1.0358 << 1.20
threshold. Decision rule FAIL on both criteria.

The Phase 2 +0.004876 full-tape expectancy was a weighted average of
a sharply bimodal regime distribution: 2024 strongly negative,
2025-2026 marginally positive. No chronological window combines a
positive avg_pnl AND a PF approaching the +1.20 robustness threshold.
Even the passing windows top out at PF=1.087, well below the bar for
robust live deployment.

Walk-forward correctly surfaced what Phase 2 averaging masked.

Recommendation per scoping memo §4: Tier 4 (signal-shape redesign).
VWR USTEC.F retune track CLOSED. Engine stays disabled at
engine_init.hpp:608 (g_vwap_rev_nq.enabled = false, S68 stop-bleed).

Driver rewritten this session to stream one window at a time after
v1 ran out of disk holding all 4 splits open concurrently. Peak temp
disk drops from ~4.4 GB to ~1.1 GB.

Tier 4 candidates queued for future work if VWR USTEC.F is revisited:
  - daily volatility regime gate (motivated by w1 low-trade anomaly)
  - multi-timeframe VWAP anchoring
  - direction-conditional entry filter
None scoped here."
git push origin main
```
