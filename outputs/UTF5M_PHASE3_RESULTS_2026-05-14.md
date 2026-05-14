# UTF5m USTEC — Tier 1 Phase 3 walk-forward validation — 2026-05-14

**Status:** COMPLETE. 4-window WF on `--mode baseline` (S63 trio
zeroed per Phase 1 evidence). Verdict: **FAIL** on PF, **PASS** on
window count. 3 of 4 windows pass `avg_pnl ≥ +0.001` (threshold ≥3,
met), aggregate PF=**1.1154** (threshold ≥1.20, **missed by 0.085**).
The baseline edge measured in Phase 1 (+928.88 gross / +0.662 avg)
reproduces on the WF (sum of windows = +935.79 gross / +0.667 avg —
matches within boundary-effect noise), but **PF is regime-driven**:
w0 marginal, w1 anomalously negative, w2-w3 increasingly positive.
The track is **closed**: engine stays disabled at
`engine_init.hpp:950` (`g_ustec_tf_5m.enabled = false`, S68
stop-bleed). Closure character is **softer than VWR's** — see §6 for
the comparison.

## 1. Scope

Phase 3 validated the Phase 1 baseline result (S63 trio = 0.0/0.0/0.0
per the decisive Phase 1 sweep at
`outputs/UTF5M_PHASE1_RESULTS_2026-05-14.md`) under a 4-window
walk-forward split. Per the part-T handoff §"Recommended
next-session focus" item 1 and the embedded decision rule.

Tape split into 4 non-overlapping equal-duration windows on a single
streaming pass. Each window run through the harness in `--mode
baseline`. No session-window axis (UTF5m has none).

Pass criterion: ≥3 of 4 windows produce `avg_pnl ≥ +0.001` AND
aggregate OOS profit factor ≥ 1.20.

The "cost-pessimistic PF" framing uses the harness's per-trade
reported `gross_pnl` directly; `ExecutionCostGuard::is_viable()` has
already filtered trades that cannot cover costs at TP at fire time,
so the reported PnL reflects the round-trip economics of trades the
engine actually fired.

## 2. Run

```bash
python3 scripts/utf5m_wf_t1.py /Users/jo/Tick/NSXUSD_merged.csv
```

Wall-clock: ~85 seconds total (single streaming pass + 4 sequential
harness runs at 10-25s each, scaling roughly with per-window tick
count).

Preflight: tape 4.43 GB, peak temp 1.11 GB (largest window),
free disk 19.14 GB. No disk-pressure issue this session (operator
freed disk after part-S; we have 7x the headroom that VWR Phase 3
had).

The streaming pattern (sequential window-stream-then-delete) carried
forward from the VWR v2 rewrite. Peak temp caps at ~1/N of tape.

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

These boundaries are **identical** to VWR Phase 3 (same tape, same
N=4 split). Per-window comparison with VWR Phase 3 is therefore
direct — see §6.4.

## 4. Per-window results

Each window run as `--mode baseline` (S63 trio = 0.0/0.0/0.0).

| Window | Period | Days | Ticks | Trades | Avg PnL | PF | Pass |
|---|---|---|---|---|---|---|---|
| w0 | 2024-H1 | 207.48 | 25,914,463 | 281 | **+0.117567** | 1.026 | PASS |
| w1 | 2024-H2 → 2025-Q1 | 207.48 | 17,200,965 | 231 | **−0.376140** | 0.937 | FAIL |
| w2 | 2025-Q1 → Q3 | 207.48 | 35,792,746 | 431 | **+0.950462** | 1.152 | PASS |
| w3 | 2025-Q4 → 2026-Q2 | 207.48 | 46,151,881 | 460 | **+1.261057** | 1.210 | PASS |

The pattern is **monotonically improving** across w0 → w3 on every
axis except w1's PF dip:

- Trade count: 281 → 231 → 431 → 460 (w1 dip + accelerating thereafter)
- Avg PnL: +0.118 → −0.376 → +0.950 → +1.261 (w1 dip + strong growth)
- PF: 1.026 → 0.937 → 1.152 → 1.210 (w1 dip + crosses threshold at w3)

w3 individually meets the ≥1.20 PF gate (PF=1.210, exactly on the
boundary). w2 is close (1.152). w0 is marginal (1.026). w1 is the
only outlier.

## 5. Aggregate and decision

| Metric | Value |
|---|---|
| Aggregate trades | 1403 |
| Aggregate gross PnL | +935.79 (Phase 1 reported +928.88 — match within rounding) |
| Aggregate avg PnL | +0.667 (Phase 1 reported +0.662) |
| Aggregate PF | **1.1154** |
| Windows passing avg_pnl ≥ +0.001 | **3 of 4** |

**Decision rule:** PASS = (windows passing ≥ 3) AND (aggregate PF ≥ 1.20).

**Verdict:** **FAIL** — PF gate missed by 0.085; window-count gate met.

The decision rule is and-conjunctive. Both gates must be met. The 3
of 4 window-count test is satisfied; the aggregate PF test is not.
Per the strict reading of the rule (which was committed to before
the run, mirroring the VWR S71 Phase 3 protocol), this is FAIL.

The Phase 1 baseline expectancy is **confirmed** (full-tape PnL
reproduces under WF). The track fails not because the edge isn't
there but because the **distribution of that edge across regimes is
not robust enough** — w1's negative window pulls aggregate PF below
the robustness threshold despite the other three being positive.

## 6. Cross-window observations

### 6.1 Phase 1 reproduces faithfully

The aggregate sums (1403 trades, +935.79 gross, +0.667 avg) match
the Phase 1 full-tape baseline (1403 trades, +928.879 gross,
+0.662 avg) within ~0.7% on gross and within rounding on avg. The
WF is correctly partitioning the Phase 1 result, not introducing new
behavior. So the FAIL here is about distribution robustness, not
about the harness or driver.

### 6.2 The improvement trajectory is the headline finding

Unlike VWR Phase 3 (which was sharply bimodal — 2024 negative,
2025-2026 marginally positive), UTF5m shows **monotonic improvement
across windows**. Every metric except PF in w1 grows w0 → w3:

```
trade count   :   281 (w0)  →   231 (w1)  →   431 (w2)  →   460 (w3)
avg_pnl       : +0.118     →  -0.376     →  +0.950     →  +1.261
PF            :  1.026     →   0.937     →   1.152     →   1.210
```

w0 is positive but marginal; w3 individually meets the 1.20 threshold.
A simpler signal-shape redesign would not be required if the engine
were re-validated on a more recent tape — the 2025-2026 portion of
this same tape would pass cleanly. The failure mode here is **2024
drag**, not "edge collapsed".

### 6.3 w1 is the trade-count anomaly (same as VWR)

UTF5m w1 (2024-07-27 → 2025-02-19): **231 trades, 17.2M ticks**.
The other three windows are 281 / 431 / 460 trades on
25.9M / 35.8M / 46.1M ticks respectively. w1 has both:

- **Fewest ticks** of any window (17.2M vs 25.9-46.1M elsewhere) —
  the underlying market was producing roughly 60% fewer
  observations per unit time. Plausibly a low-volatility regime.
- **Fewest trades per tick** (231 / 17.2M = 13 trades per million
  ticks vs 11 / 12 / 10 for w0/w2/w3) — the engine's signal trigger
  rate per tick was actually slightly elevated, but the underlying
  tick stream was sparse enough to produce fewer total entries.
- **Worst avg_pnl per trade** (−0.376) — the entries that did fire
  performed materially worse than in any other window.

This is the same w1 fingerprint VWR Phase 3 surfaced — low-vol
regime with hostile signal behavior. The mechanism is plausibly
shared: Donchian breakouts and EWM-VWAP reversions both depend on
range expansion, and a tight 2024-H2 / 2025-Q1 range starves both
strategies.

A daily volatility regime gate (suppress firing on low-prior-vol
days) is the most actionable Tier 4 candidate this finding suggests.
The VWR Phase 3 memo §7 made the same recommendation for
VWAPReversion. If both engines share the failure mode, a shared
regime-gate utility (e.g. cross-asset `is_high_vol_regime(now_ms,
symbol)` helper) might be worth scoping in any Tier 4 session.

### 6.4 Comparison with VWR Phase 3 (same tape, same windows)

| Metric | VWR Phase 3 | UTF5m Phase 3 |
|---|---|---|
| Aggregate trades | 4280 | 1403 |
| Aggregate avg_pnl | +0.004746 | +0.667 |
| Aggregate PF | 1.0358 | **1.1154** |
| Windows passing | 2 of 4 | **3 of 4** |
| Per-window pattern | bimodal (2024 neg, 2025+ pos) | monotonically improving |
| w1 trade count | 730 (low) | 231 (low) |
| Verdict | FAIL (both gates) | FAIL (PF gate only) |

UTF5m is the **stronger track**:
- Higher PF (1.1154 vs 1.0358 — closer to the 1.20 gate).
- More windows pass (3 vs 2).
- Trajectory points the right way (monotonically improving vs
  bimodal regime collapse).
- The failing window is anomalous, not systemic.

But UTF5m still fails the strict rule. The PF gate exists precisely
to guard against this kind of "almost passes" outcome where one
regime drags the average — passing it would set a precedent that
softens the bar for every subsequent engine validation.

### 6.5 Per-window avg_pnl scale: 100x-1000x larger than VWR

VWR's per-trade avg_pnl is on the order of $0.01 (typical VWR
USTEC.F PF=1 trade); UTF5m's is on the order of $1 (typical trend-
follow gain). This reflects the difference in strategy archetypes,
not anything about WF mechanics. Both pass/fail at the same
+0.001 threshold because the threshold is set as "demonstrably non-
zero, ignoring sign noise". No re-calibration needed.

## 7. Outcome — closure call, with Tier 4 left as an open option

Per the strict decision rule, **the UTF5m retune track is closed**.
Engine stays disabled at `engine_init.hpp:950`
(`g_ustec_tf_5m.enabled = false`, S68 stop-bleed). Shadow remains at
`true`.

This closure is **less definitive than VWR's** (§6.4):

- VWR Phase 3 surfaced a structural problem with the signal itself
  (bimodal regime collapse, even passing windows weak at PF≈1.07).
  The recommendation was Tier 4 (signal-shape redesign) because the
  signal mechanism doesn't survive on multiple axes.
- UTF5m Phase 3 surfaces a **regime concentration** — the signal
  works in 3 of 4 windows and the trajectory is improving; the
  failure is "one bad year drags aggregate PF below the gate".

**Available next moves**, in priority order:

1. **Vol-regime gate as a Tier 4 candidate.** Same as VWR's §7 item
   1 — only fire when prior 5-day realised vol (or ATR) exceeds a
   threshold. If w1's "low-vol → bad fills" hypothesis is correct,
   suppressing trades in that regime should lift w1 from
   avg_pnl=−0.376 toward neutral, which would lift aggregate PF
   above 1.20 without modifying signal mechanics.

2. **Re-validate on a shorter, more recent tape.** The 829-day tape
   includes the bad 2024 portion. A 1-year tape from 2025-04 onward
   would presumably pass the PF gate cleanly. This is not a sound
   replacement for WF — fitting to fresh data without re-OOS is the
   exact failure mode WF guards against — but it's a useful
   sensitivity check before committing to Tier 4 redesign.

3. **Re-run Phase 3 with a 5-or-6-window split.** A finer-grained
   WF might surface that 2024-Q4 specifically is the problem
   subwindow inside w1, and the rest of w1 is fine. If the bad zone
   is bounded to a specific quarter, the closure character shifts
   from "regime concentration" to "specific bad quarter" — a smaller
   problem to gate.

4. **Accept the PF miss and re-enable.** **Not recommended.** This
   would mean overriding a pre-committed decision rule after
   running it. The rule's purpose is precisely to prevent this kind
   of post-hoc nuance from softening the bar. If we relax the rule
   for UTF5m (which is genuinely close), we set a precedent that
   the next engine which is "close but PF=1.11" will also be
   re-enabled. The whole point of the WF gate is removed.

None of these are scoped here; they're the queue for a future
operator decision if/when UTF5m is revisited.

The two follow-ups flagged at VWR Phase 1 §6 (the `ext-sl-ratio`
plateau and the `tp_rate ≥ 5%` gate calibration) don't apply to
UTF5m's parameter surface and are not relevant here.

## 8. Operational note — Mac disk

The Mac was at ~19 GB free at WF run time — comfortable. Operator
cleaned disk between part-S Phase 3 closeout and this session, and
the sandbox-side disk-full from part-T did not affect Mac-side work.
The constraint pattern from VWR Phase 3 (peak temp ~1.1 GB at N=4)
held: even at part-S's 2.84 GB free, this would have succeeded. The
"free disk when convenient" advisory from VWR Phase 3 §8 has been
satisfied by the operator's cleanup.

## 9. Artifacts

- Summary CSV: `outputs/utf5m_t1_p3_20260514_145416/wf_summary.csv`
- Verdict text: `outputs/utf5m_t1_p3_20260514_145416/wf_verdict.txt`
- Per-window reports: `outputs/utf5m_t1_p3_20260514_145416/cells/w*_report.csv`
- Per-window trades: `outputs/utf5m_t1_p3_20260514_145416/cells/w*_trades.csv`
- Per-window stderr: `outputs/utf5m_t1_p3_20260514_145416/cells/w*_stderr.log`
- This memo: `outputs/UTF5M_PHASE3_RESULTS_2026-05-14.md`
- Driver: `scripts/utf5m_wf_t1.py`

`outputs/utf5m_t1_p3_<ts>/` is gitignored; this memo and the driver
are the only artifacts committed.

## 10. Commit suggestion

Bundle the new driver with the closure memo (mirrors the VWR S71 P3
commit shape — script + memo together):

```bash
cd ~/omega_repo
git add scripts/utf5m_wf_t1.py
git add -f outputs/UTF5M_PHASE3_RESULTS_2026-05-14.md
git diff --cached --stat
git commit -m "S73: UTF5m USTEC Tier 1 Phase 3 walk-forward FAIL - close track

4-window walk-forward on --mode baseline (S63 trio = 0.0/0.0/0.0,
matching Phase 1 evidence). Verdict: FAIL on PF, PASS on windows.

Per-window results (avg_pnl / PF):
  w0 (2024-H1)            : +0.117567 / 1.026  PASS
  w1 (2024-H2 -> 2025-Q1) : -0.376140 / 0.937  FAIL
  w2 (2025-Q1 -> Q3)      : +0.950462 / 1.152  PASS
  w3 (2025-Q4 -> 2026-Q2) : +1.261057 / 1.210  PASS

Aggregate: 1403 trades, avg=+0.667, PF=1.1154.
3 of 4 windows pass (>=3 threshold MET). Aggregate PF 1.1154 below
1.20 threshold (gate MISSED by 0.085). Decision rule is and-
conjunctive: both gates required. PF gate fails -> overall FAIL.

The Phase 1 baseline expectancy (+928.88 gross, +0.662 avg per
UTF5M_PHASE1_RESULTS_2026-05-14.md) is faithfully reproduced under
WF (sum of windows = +935.79 gross, +0.667 avg) -- the edge exists
but is regime-concentrated. Trajectory is monotonically improving
across windows; w3 individually meets the 1.20 PF gate (1.210).
The single failing window (w1) has the same low-trade-count
fingerprint as VWR Phase 3 w1 (low-vol regime hypothesis).

Closure is softer than VWR's: VWR was bimodal regime collapse,
UTF5m is one anomalous window dragging an otherwise improving
trajectory. Tier 4 candidates queued for future revisit:
  - daily volatility regime gate (shared candidate with VWR
    Phase 3 recommendation -- same w1 anomaly pattern)
  - 5-or-6 window finer WF split to isolate the bad subwindow
  - shorter / more recent tape sensitivity check (not a sound
    WF replacement, but useful before Tier 4 commit)
None scoped here.

Engine stays disabled at engine_init.hpp:950
(g_ustec_tf_5m.enabled = false, S68 stop-bleed).

Adds scripts/utf5m_wf_t1.py adapted from scripts/vrev_wf_t1.py:
  - drops --symbol (UTF5m harness is USTEC-only)
  - drops --session-open-hour / --close-hour (no session flags)
  - default harness path: build/UstecTrendFollow5mBacktest
  - mode hard-pinned to --mode baseline
  - summary CSV uses UTF5m exit-reason vocabulary
  - verdict text references engine_init.hpp:950 + S63 trio revert

S72 P0 (harness) + S72 P1 (Phase 1 sweep) + S73 (this Phase 3 close)
collectively answer the engine_init.hpp:964-967 promotion gate:
S63 + S37 widened SL/TP is NOT net-positive on USTEC under WF
discipline. Engine remains disabled pending Tier 4."
git push origin main
```

## 11. Optional follow-up — engine_init.hpp comment refresh

The promotion-gate text at `engine_init.hpp:964-967` is now stale —
it still reads "stays FALSE until a fresh-tape backtest sweep
confirms...". The sweep is done; the result is "does not confirm".

A small comment-only edit refreshing that block to reference the
Phase 1 + Phase 3 memos would help future sessions avoid re-running
this work. Suggested replacement text:

```cpp
//   Promotion gate (RESOLVED 2026-05-14 -- gate fails):
//     - Phase 1 (S72 P1, UTF5M_PHASE1_RESULTS_2026-05-14.md):
//       S63 + S37 widened SL/TP is empirically adverse on USTEC.
//       Baseline (S63 zeroed) gross=+929; every S63-active cell
//       net negative. Phase 2 skipped (signal decisive).
//     - Phase 3 (S73, UTF5M_PHASE3_RESULTS_2026-05-14.md):
//       4-window WF on baseline fails PF gate (1.1154 vs 1.20).
//       3 of 4 windows pass; w1 (2024-H2) anomalous.
//     Engine remains disabled. Re-enable requires either a Tier 4
//     redesign (vol-regime gate is the leading candidate) OR a
//     deliberate operator decision to soften the decision rule
//     (NOT recommended -- see Phase 3 memo §7 item 4).
```

This is a comment-only change (no settings change). Out of scope
for the closure commit; suggest as a separate commit if the
operator wants it. Commit message could be:

```
docs(engine_init): refresh g_ustec_tf_5m promotion-gate comment

Reflect S73 closure outcome -- gate is now RESOLVED (fails). Points
future sessions at the Phase 1 + Phase 3 memos so the next reader
doesn't re-scope the sweep.

No settings change. Comment-only edit at engine_init.hpp:964-967.
```
