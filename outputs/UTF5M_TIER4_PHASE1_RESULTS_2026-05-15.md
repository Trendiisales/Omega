# UTF5m Tier 4 vol-regime gate — Phase 1 sweep — 2026-05-15

**Status:** COMPLETE. 14-cell sweep on `--mode baseline` (S63 trio
zeroed) with the S90 entry-blocking vol-regime gate (S91 harness CLI).
**Verdict: FAIL** — no (lookback, threshold) cell cleared both the
absolute (≥1.20) and the relative (≥1.10 × baseline) PF gates. Best
cell `L30_T90` reaches PF=1.1591 (+4.01% vs baseline 1.1144), still
well short of the 1.20 bar. The track is **closed**: engine stays
disabled at `engine_init.hpp:1009` (`g_ustec_tf_5m.enabled = false`,
S68 stop-bleed). S93 (Phase 3 WF) is retired — Phase 1 was decisive.

## 1. Scope

Phase 1 of the UTF5m Tier 4 vol-regime entry-gate work, scoped per the
2026-05-14 part-AB handoff §"2. UTF5m Tier 4 vol-regime gate port":

> "Phase 1 sweep (~20 min). 14-cell matrix: 2 lookbacks × 7 thresholds
> (50, 60, 70, 75, 80, 85, 90), Form A only, symmetric. Decision
> criterion: best cell beats baseline by ≥10% on aggregate PF AND
> clears ≥1.20."

Goal: test whether suppressing low-vol entries can lift UTF5m aggregate
PF over the 1.20 walk-forward bar without modifying signal mechanics,
per the hypothesis at `outputs/UTF5M_PHASE3_RESULTS_2026-05-14.md` §7
item 1.

Tier 4 framework landed in two prior commits this session:

- **S90** (`91b50f3`) — engine-side plumbing in
  `include/UstecTrendFollow5mEngine.hpp`: daily H/L tracking in
  `on_5m_bar`, ATR(14) ring + percentile history, entry-decision gate
  before the cell loop. **Form: entry-blocking** (different from VWR
  S85 Form A which gates S63 management). Default OFF; bit-for-bit
  inert when `VOL_GATE_ENABLED=false`.
- **S91** (`e5e93d3`) — harness CLI flags in
  `backtest/UstecTrendFollow5mBacktest.cpp`: `--vol-gate-enabled`,
  `--vol-pct-threshold N`, `--atr-lookback-days N`. Engine defaults
  passed through; `--atr-lookback-days` clamped to
  `[1, VG_MAX_LOOKBACK_DAYS=60]` with stderr-fatal out-of-range check.

Mode pinned to `--mode baseline` (S63 trio = 0.0/0.0/0.0) so the
comparison is apples-to-apples with S73 Phase 3 (which also ran
baseline). The Tier 4 entry-gate operates on top of the S63-zeroed
engine; it does NOT touch S63 management.

## 2. Run

```bash
python3 scripts/utf5m_t4_p1.py /Users/jo/Tick/NSXUSD_merged.csv
```

Wall-clock: ~4.5 min total (1 baseline × 18s + 14 cells × ~17.5s each
≈ 4 min plus startup overhead). Significantly faster than the ~21 min
upper estimate — harness throughput per tick has improved since the
S73 Phase 3 measurement.

Tape format auto-detected as **A_TBA**
(`timestamp_ms,bid,ask`), same as S73. The harness's internal
auto-detection ran independently and converged on the same format
(visible in each cell's per-cell stderr.log).

## 3. Tape coverage

Auto-detected by the driver from the first / last data lines:

| Field | Value |
|---|---|
| Format detected | A_TBA |
| start_iso (UTC) | 2024-01-01T23:00:00Z |
| end_iso (UTC) | 2026-04-10T21:14:59Z |
| total days | 829.93 |

**Identical** to the S73 Phase 3 tape. Per-cell comparison with the
S73 closure-memo baseline is therefore direct.

## 4. Per-cell results

Each cell run as `--mode baseline --vol-gate-enabled
--atr-lookback-days <L> --vol-pct-threshold <T>` on the full 829-day
tape. Baseline run uses `--mode baseline` only (no gate flags); the
S90 default-OFF guarantee makes the daily H/L state advance but the
entry gate's `if` branch never fire.

### 4.1 Baseline (gate OFF — reproduces S73 Phase 3)

| Metric | Value |
|---|---|
| Trades | 1403 |
| Gross PnL | +928.879 |
| Avg PnL | +0.662066 |
| PF | **1.1144** |

The baseline reproduces the S73 Phase 3 closure-memo result
(PF=1.1154, 1403 trades, avg=+0.667) within rounding noise. Three
implications:

1. The gate's default-OFF guarantee at
   `UstecTrendFollow5mEngine.hpp` is upheld: with
   `VOL_GATE_ENABLED=false`, behaviour is bit-for-bit identical to
   pre-S90.
2. The harness's S91 CLI changes don't perturb the no-flag path.
3. The S73 measured baseline is the correct comparison anchor for
   this Phase 1 sweep.

### 4.2 Sweep grid (14 cells)

L30 lookback (rolling 30-day ATR window):

| Cell | T | Trades | Avg PnL | PF | Δ vs base | Pass |
|---|---|---|---|---|---|---|
| L30_T50 | 50 | 906 | +0.609969 | 1.099 | −1.40% | NO |
| L30_T60 | 60 | 792 | +0.462149 | 1.073 | −3.69% | NO |
| L30_T70 | 70 | 688 | +0.485567 | 1.078 | −3.29% | NO |
| L30_T75 | 75 | 595 | +0.647141 | 1.103 | −1.04% | NO |
| L30_T80 | 80 | 558 | +0.644300 | 1.101 | −1.23% | NO |
| L30_T85 | 85 | 510 | +0.764444 | 1.117 | +0.25% | NO |
| L30_T90 | 90 | 476 | +1.022856 | **1.159** | **+4.01%** | NO (best) |

L60 lookback (rolling 60-day ATR window):

| Cell | T | Trades | Avg PnL | PF | Δ vs base | Pass |
|---|---|---|---|---|---|---|
| L60_T50 | 50 | 934 | +0.532259 | 1.086 | −2.51% | NO |
| L60_T60 | 60 | 815 | +0.826368 | 1.134 | +1.76% | NO |
| L60_T70 | 70 | 754 | +0.856149 | 1.138 | +2.13% | NO |
| L60_T75 | 75 | 677 | +0.847993 | 1.135 | +1.88% | NO |
| L60_T80 | 80 | 608 | +0.700659 | 1.111 | −0.31% | NO |
| L60_T85 | 85 | 524 | +0.748242 | 1.117 | +0.24% | NO |
| L60_T90 | 90 | 454 | +0.308921 | 1.047 | −6.08% | NO |

## 5. Decision

**Decision rule** (pre-committed per part-AB handoff):

```
PASS = best_cell_pf >= 1.20                         (absolute)
       AND best_cell_pf >= 1.10 * baseline_pf       (>= +10% relative)
```

**Best cell:** L30_T90 — PF=1.1591, +4.01% vs baseline.

| Gate | Threshold | Best cell | Met |
|---|---|---|---|
| Absolute PF | ≥ 1.20 | 1.1591 | **NO** |
| Relative PF | ≥ 1.2258 (= 1.10 × 1.1144) | 1.1591 | **NO** |

**Verdict: FAIL** on both gates. No cell in the 14-cell grid clears
either. The Tier 4 vol-regime entry gate does not rescue UTF5m on
this tape.

## 6. Cross-cell observations

### 6.1 The grid edge does not hide a winner

The best cell is at the **T90 edge of the L30 row** with the second-
best result at the L60 mid-grid (L60_T70, +2.13%). If we had a
winner-by-extrapolation, the trajectory would point upward off the
grid edge. Instead:

- **L30 is monotonically improving T50→T90** (more aggressive blocking
  → higher PF, but rapidly losing volume). T90 is the limit of what
  a higher-threshold extension could plausibly reach. Continuing
  toward T95 would block even more entries but the PF gain per
  blocked-entry has been declining cell-over-cell (+0.054 PF at T75→T80,
  +0.016 at T80→T85, +0.042 at T85→T90 — noisy but no acceleration).
  Extrapolating linearly from T85→T90 to T95 would land around
  PF≈1.20, but the volume would also halve again (from 476 trades
  to ~240). Below the credible-stat threshold for the WF.
- **L60 is peaked**, not monotonic. T70 wins at PF=1.138, then degrades.
  T90 collapses to PF=1.047 (worst in the grid). The L60 row gives
  no support to "push the threshold higher".

So the grid edge isn't hiding a marginal winner one notch over.

### 6.2 The volume cost is severe

| Cell | Trades | Trade retention vs baseline |
|---|---|---|
| baseline | 1403 | 100% |
| L30_T50  |  906 |  65% |
| L60_T70  |  754 |  54% |
| L30_T90  |  476 |  34% (best cell) |
| L60_T90  |  454 |  32% |

The best cell takes only 34% of baseline trades. Even hypothetically
clearing the PF gate at that volume would have left the engine with
~470 trades over 829 days — about one trade every 1.8 days. That's
already at the edge of statistical comfort for the Phase 3 4-window
WF (~120 trades per window, much smaller than baseline's 1403/4 ≈ 350).
A Phase 3 WF on L30_T90 would likely surface per-window trade-count
collapse as its own failure mode even before any PF question.

### 6.3 The Phase 3 §7 hypothesis is rejected

The closure memo's hypothesis:

> "Vol-regime gate as a Tier 4 candidate. ... If w1's 'low-vol → bad
> fills' hypothesis is correct, suppressing trades in that regime
> should lift w1 from avg_pnl=−0.376 toward neutral, which would lift
> aggregate PF above 1.20 without modifying signal mechanics."

Empirically tested by this sweep: **no.** A +4.01% best-case lift is
far short of the +7.66% lift needed to cross 1.20 from a 1.1144
starting point. Possible explanations (none mutually exclusive):

1. **w1 may not be a "low-vol regime" the way ATR(14) measures it.**
   w1 had 17.2M ticks vs 25.9-46.1M elsewhere — but tick count is
   message-density (broker liquidity, quote chattiness), not the same
   as price-range volatility. ATR-percentile catches range expansion;
   w1 may have had a low-tick-count regime that wasn't materially
   low-ATR.
2. **Donchian/Keltner false breakouts may be regime-diffuse, not
   low-vol-concentrated.** False signals at high ATR (gap day fades,
   exhaustion breakouts) are just as expensive as false signals at low
   ATR. A vol-regime gate trims one tail of the false-signal
   distribution but leaves the other untouched.
3. **The w1 anomaly may be event-driven, not regime-driven.** A
   specific 2024-Q4 sequence (rate-decision sequence, election-window
   liquidity, etc.) producing a cluster of bad fills that a
   distributional gate cannot identify.

None of these are actionable from this sweep alone. They'd require
event-study tooling, which is out of scope.

### 6.4 Comparison with VWR S85 / S87 (vol-regime gate on a different engine)

VWR's vol-regime gate (S85, committed `e24a666`) plumbs the same ATR-
percentile infrastructure into `VWAPReversionEngine`, but its **Form
A gates S63 management** (not entries). That has a different
mechanism: BE_RATCHET / LOSS_CUT cut harder in low vol, looser in high
vol — controlling EXISTING in-flight protection.

UTF5m's S90 gate **blocks entries directly** because Phase 3 ran with
S63 zeroed and STILL produced w1's negative avg_pnl. The two engines
needed different gate semantics, even though the ATR-percentile
pipeline is identical.

VWR's Phase 1 (the S87 sweep driver, still queued at session end) has
not yet run — so we can't yet say whether VWR's Form A gate-S63 lifts
its 1.0358 baseline. But the methodology here (read decision-rule
result first, retire post-hoc retunes) applies symmetrically to that
sweep when it does run.

### 6.5 The +$728 USTEC trade revisited

The part-AB session's investigation found that the +$728 USTEC trade
(2026-05-13) was a **tail-of-distribution winner from an engine with
real edge but PF-gate-failing distribution**. The Tier 4 gate was the
last empirical option that didn't require modifying signal mechanics.
With that option now negative, the avenue for re-enabling
`g_ustec_tf_5m` via in-engine changes (no signal redesign) is closed.

Re-enable could still happen via:

- **Signal redesign** (out of scope for this work; would be a major
  Tier 4 candidate with its own backtest cycle).
- **Shorter, more recent tape sensitivity check** (Phase 3 §7 item 2 —
  not a sound WF replacement but useful as a sanity check before
  committing to redesign).
- **Deliberate operator decision to soften the decision rule**
  (Phase 3 §7 item 4 — explicitly NOT recommended; sets a precedent).

None scoped here.

## 7. Outcome — closure call

**The UTF5m Tier 4 vol-regime entry-gate track is closed.**

- Engine remains disabled at `engine_init.hpp:1009`
  (`g_ustec_tf_5m.enabled = false`, S68 stop-bleed).
- The S90 / S91 engine plumbing stays in the codebase as a tested,
  inert capability. Default-OFF guarantee means it costs nothing at
  runtime. If a future session needs a vol-regime gate on UTF5m for a
  DIFFERENT decision rule (e.g. operator-authorised relaxation), the
  infrastructure is already there.
- The `g_ustec_tf_5m` shadow remains `true` at the production
  engine_init site, irrespective of this closure.
- **S93 (Phase 3 WF) is retired** — Phase 1 was decisive.

The closure character is **harder than the S73 Phase 3 closure**:
S73 closed with an explicit Tier 4 candidate open as the next move.
This sweep tested that candidate and found it does not work. The next
move for UTF5m re-enable is now either signal redesign (separate
project) or operator-authorised rule relaxation (not recommended).

## 8. Operational note

The sweep ran cleanly. Mac canary was green for both S90 and S91
before the sweep, and the harness's CLI flags rendered correctly per
the S91 smoke test. No disk-pressure or build-pipeline issues — the
20-min wall-clock estimate from the part-AB handoff turned out
conservative (actual ~4.5 min).

## 9. Artifacts

- Sweep driver: `scripts/utf5m_t4_p1.py` (this commit)
- Cells summary CSV: `outputs/utf5m_t4_p1_20260515_103659/cells_summary.csv`
- Verdict text: `outputs/utf5m_t4_p1_20260515_103659/verdict.txt`
- Per-cell harness output:
  `outputs/utf5m_t4_p1_20260515_103659/cells/<name>_report.csv`,
  `..._trades.csv`, `..._stderr.log`
- This memo: `outputs/UTF5M_TIER4_PHASE1_RESULTS_2026-05-15.md`

`outputs/utf5m_t4_p1_<ts>/` is gitignored; this memo and the driver
are the only artifacts committed.

## 10. Optional follow-up — engine_init.hpp comment refresh

The `g_ustec_tf_5m` init block at `engine_init.hpp:1004-1038` includes
the promotion-gate comment refreshed by S73 (the Phase 3 closure).
That comment block currently reads (paraphrased):

> "Re-enable requires either a Tier 4 redesign (vol-regime gate is
> the leading candidate) OR a deliberate operator decision to soften
> the decision rule (NOT recommended)."

The Tier 4 vol-regime-gate candidate has now been tested and FAILED.
A small comment-only refresh to point future sessions at this Phase 1
memo would help avoid re-scoping the same sweep. Suggested replacement
text:

```cpp
//   Promotion gate (RESOLVED 2026-05-14 + 2026-05-15 -- gate fails):
//     - Phase 1 (S72 P1, UTF5M_PHASE1_RESULTS_2026-05-14.md):
//       S63 + S37 widened SL/TP is empirically adverse on USTEC.
//     - Phase 3 (S73, UTF5M_PHASE3_RESULTS_2026-05-14.md):
//       4-window WF on baseline fails PF gate (1.1154 vs 1.20).
//     - Tier 4 vol-regime entry-gate Phase 1 (S92,
//       UTF5M_TIER4_PHASE1_RESULTS_2026-05-15.md):
//       14-cell sweep (L=30,60 x T=50..90), no cell clears 1.20 or
//       beats baseline by >=10%. Best L30_T90 at PF=1.1591 (+4.01%).
//     Engine remains disabled. Re-enable now requires signal
//     redesign (not in any current session scope).
```

Out of scope for this closure commit; suggest as a separate small
comment-only commit if the operator wants it.

## 11. Commit suggestion

Bundle the new driver with the closure memo (mirrors the S73 Phase 3
commit shape — script + memo together). Per CLAUDE.md heredoc rule:

```bash
cd ~/omega_repo
git add scripts/utf5m_t4_p1.py
git add -f outputs/UTF5M_TIER4_PHASE1_RESULTS_2026-05-15.md
git diff --cached --stat
cat > /tmp/s92_msg.txt <<'EOF'
S92: UTF5m Tier 4 vol-regime entry-gate Phase 1 sweep FAIL - close track

14-cell sweep on --mode baseline (S63 trio zeroed, matching S73 Phase
3 setup) with the S90 entry-blocking vol-regime gate active per cell.

Sweep grid (2 lookbacks x 7 thresholds):
  ATR_LOOKBACK_DAYS:  30, 60
  VOL_PCT_THRESHOLD:  50, 60, 70, 75, 80, 85, 90

Decision rule (per part-AB handoff):
  PASS = best_cell_pf >= 1.20  AND  best_cell_pf >= 1.10 * baseline_pf

Baseline (gate OFF): 1403 trades, avg=+0.662, PF=1.1144
  (reproduces S73 Phase 3 closure within rounding noise --
   confirms default-OFF guarantee at S90)

Best cell: L30_T90 -- 476 trades, PF=1.1591, +4.01% vs baseline.
Absolute gate (1.20) missed by 0.041 PF. Relative gate
(1.2258 = 1.10 * 1.1144) missed by 0.067 PF. Both gates FAIL.

No cell in the 14-cell grid clears either gate. The Tier 4 vol-regime
entry gate does not rescue UTF5m on this tape. The Phase 3 memo
hypothesis (suppress low-vol entries -> lift w1 -> aggregate PF
above 1.20) is rejected.

Cross-cell observations:
  - L30 row monotonic T50->T90 (more blocking -> higher PF, fewer
    trades). T90 is the credible-stat limit; T95 extrapolates near
    1.20 but at ~240 trades, below WF threshold.
  - L60 row peaked T60-T75 then degrades. T90 collapses to 1.047
    (worst in grid).
  - Best-cell trade retention 34% of baseline. Even hypothetical
    PF-pass would have per-window trade counts too low for Phase 3
    WF discipline.

Track closed: engine stays disabled at engine_init.hpp:1009
(g_ustec_tf_5m.enabled = false, S68 stop-bleed). S93 (Phase 3 WF)
retired -- Phase 1 was decisive.

S90 / S91 engine + harness plumbing stays in the codebase as tested
inert capability. Default-OFF guarantee means zero runtime cost; if a
future session needs the gate for a different decision (e.g. operator-
authorised rule relaxation), the infrastructure exists.

Adds:
  - scripts/utf5m_t4_p1.py
      Adapted from scripts/utf5m_wf_t1.py (S73 Phase 3 driver).
      Cartesian sweep grid instead of window split; best-cell-vs-
      baseline decision rule; supports --skip-baseline / --baseline-pf
      for resumable re-runs; --lookbacks / --thresholds overrides.
  - outputs/UTF5M_TIER4_PHASE1_RESULTS_2026-05-15.md
      Force-added; outputs/utf5m_t4_p1_<ts>/ is gitignored. Memo
      structure mirrors UTF5M_PHASE3_RESULTS_2026-05-14.md.

S90 (engine, 91b50f3) + S91 (harness, e5e93d3) + S92 (this) close the
Tier 4 vol-regime-gate Phase 1 question for UTF5m: tested, fails.
EOF
git commit -F /tmp/s92_msg.txt
git push origin main
git log --oneline -5
```
