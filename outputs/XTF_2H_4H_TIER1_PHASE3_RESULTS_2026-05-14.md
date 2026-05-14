# XTF 2h + 4h Tier 1 Phase 3 walk-forward results — closure memo

**Date:** 2026-05-14 (NZST, session part-Z, follow-up to part-Y handoff
`SESSION_HANDOFF_2026-05-14n.md` next-session priority items #1 + #2).

**Author:** Cowork session (Mac canary build + WF runs operator-side; the
sandbox bash continues dead per parts T-X, all builds and harness/driver
execution operator-side, all file edits sandbox-side).

**Verdict:** Both XTF 2h and XTF 4h **FAIL** the Phase 3 walk-forward
gate decisively. Aggregate PF is 0.72 (2h) and 0.41 (4h) vs the 1.20
threshold. Not a near-miss — the baseline engines are losing money at
the gate-test level. The more important finding is the PROVENANCE
contradiction surfaced below.

## Run metadata

- Harness binary: `./build/XauTrendFollowBacktest` (built green this
  session via `cmake --build build --target XauTrendFollowBacktest -j`
  with the part-Z 4h+d1 dispatch fill-in landed as commit `b54f60b`).
- Drivers: `scripts/xtf_2h_wf_t1.py` (part-W) + `scripts/xtf_4h_wf_t1.py`
  (part-Z, this session).
- Tape: `/Users/jo/Tick/duka_ticks/XAUUSD_2024-03_2026-04_combined.csv`
  (the part-K substitute tape; format `A_TBA` — numeric-ms epoch — auto-
  detected).
- Tape range: 2024-03-01T02:00:00Z .. 2026-04-24T20:59:58Z (784.8 days).
- N windows: 4 (evenly split, 196.2 days each).
- Engines: `XauTrendFollow2hEngine` (5 cells via `kXauTf2hCells[]`) and
  `XauTrendFollow4hEngine` (6 cells via `kXauTfCells[]`).
- Mode: `--mode baseline` (S63 trio = 0.0 / 0.0 / 0.0; class default
  state-B for both engines).
- Run timestamps: 4h at `outputs/xtf_4h_t1_p3_20260514_191218/`, 2h at
  `outputs/xtf_2h_t1_p3_20260514_191707/`.

## Per-window results

| W | start_iso | end_iso | days | 2h trades | 2h avg_pnl | 2h PF | 2h pass | 4h trades | 4h avg_pnl | 4h PF | 4h pass |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 2024-03-01 | 2024-09-13 | 196.2 | 215 | -0.460 | 0.221 | no | 240 | -0.517 | 0.220 | no |
| 2 | 2024-09-13 | 2025-03-28 | 196.2 | 218 | -0.530 | 0.219 | no | 240 | -0.725 | 0.189 | no |
| 3 | 2025-03-28 | 2025-10-10 | 196.2 | 212 | -0.079 | 0.785 | no | 238 | -0.057 | 0.893 | no |
| 4 | 2025-10-10 | 2026-04-24 | 196.2 | 201 | **+0.531** | **2.088** | **yes** | 227 | -0.735 | 0.454 | no |
| AGG | — | — | 784.8 | 846 | -0.147 | 0.724 | — | 945 | -0.506 | 0.407 | — |

## Decision rule

- 2h: 1/4 windows pass per-window `avg_pnl >= +0.001` (need >= 3),
  aggregate PF 0.724 < 1.20 → **FAIL**.
- 4h: 0/4 windows pass, aggregate PF 0.407 < 1.20 → **FAIL**.

Pre-commit pass criterion not met for either timeframe. `engine_init.hpp`
state-B values (S63 OFF) remain correct and should NOT be changed.

## What this actually shows

The drivers' boilerplate verdict-text says "S63-adverse pattern from VWR
USTEC (S71 P3) and UTF5m USTEC (S73 P3) generalises to XAU trend-follow."
**That is incorrect** and is the part-X carry-over #6 verdict-text bug
discussed in the part-Y handoff. Both runs were `--mode baseline` (S63 =
0.0 / 0.0 / 0.0), so the FAIL cannot be evidence about S63 specifically —
it is evidence that the **baseline XTF trend-follow engines have no
edge on this tape**, which is a different conclusion. S63 evaluation on
XTF is structurally moot while the baseline is underwater, because there
is no profitable signal to which the in-flight protection could add or
subtract value.

The verdict-text fix lands in a follow-up commit alongside this memo —
see `scripts/xtf_2h_wf_t1.py` / `scripts/xtf_4h_wf_t1.py` diffs.

## The PROVENANCE contradiction (load-bearing)

The PROVENANCE block at `XauTrendFollow4hEngine.hpp:7-25` documents the
4h cells as built from `edge_hunt.cpp` + `top_cells_monthly.cpp` results
on a 3-year Duka corpus 2023-09-27 .. 2025-09-26, with each cell net
positive across all three years:

```
[A] Donchian N=20 sl1.5tp3.0      n=134  WR=47%  net=+$1332  $9.94/trade
[B] InsideBar    sl2.0tp4.0       n=138  WR=44%  net=+$1124  $8.15/trade
[C] ER0.20 mom=20 sl1.5tp3.0      n=214  WR=40%  net=+$840   $3.92/trade
```

Yet WF Windows 1-3 of this run cover 2024-03 .. 2025-10, which is
**fully inside** the PROVENANCE range, and show:

- 4h Window 1 (2024-03 .. 2024-09): avg_pnl -0.517, PF 0.220
- 4h Window 2 (2024-09 .. 2025-03): avg_pnl -0.725, PF 0.189
- 4h Window 3 (2025-03 .. 2025-10): avg_pnl -0.057, PF 0.893

This contradiction needs an explanation before any further XTF S63 work
proceeds. Hypotheses to investigate, in order of prior probability:

1. **Different tape source.** PROVENANCE was built on the original 3-year
   Duka corpus (per part-K handoff, that tape is gone — the current
   substitute tape `XAUUSD_2024-03_2026-04_combined.csv` was selected as
   a replacement). Tape-source divergence is the simplest explanation:
   different broker, different aggregation, different gap handling. The
   first thing to check is whether the original PROVENANCE corpus
   produces the same numbers in `XauTrendFollowBacktest` today (if the
   tape is still around in any form).

2. **Bar-construction divergence.** PROVENANCE may have used true H4
   candles from a market-data feed; the harness aggregates ticks into
   UTC-aligned 4-hour buckets (see `h4_bucket_ms()` at
   `XauTrendFollowBacktest.cpp:367-369`). Session-boundary or
   weekend-gap handling between the two methods could differ enough
   to invert the edge.

3. **Cost / fill model divergence.** PROVENANCE states "Realistic
   bid/ask fill simulation, $0.06/RT cost subtracted, 0.01 lot." The
   harness uses the engine's own `lot` / `max_spread` settings and
   does not subtract a RT cost separately — gross_pnl is gross.
   $0.06/RT on 945 trades would be ~$57 in cost; gross PnL for the 4h
   aggregate is ~-$478 (avg_pnl -0.506 * 945 trades). Cost difference
   cannot account for the ~$500 PnL swing.

4. **Engine drift since PROVENANCE.** The cells got R:R refits in
   S33g/h/i (2026-05-11; see `XauTrendFollow4hEngine.hpp:159-185`
   PROVENANCE-extension comments). The new sl/tp combinations were
   chosen via the Pass-4/Pass-5/Pass-6 deep_dive results on what was
   presumably the same tape, so this should not explain a tape-vs-tape
   regression — but it does mean the *current* cells are not the
   PROVENANCE-validated cells, they are S33-tuned descendants.

## Window 4 divergence (regime signal)

The 2h and 4h timeframes diverge sharply on Window 4 (2025-10-10 ..
2026-04-24, the recent XAU rally):

- **2h Window 4:** avg_pnl +0.531, PF 2.088, 201 trades — passes the
  per-window threshold cleanly.
- **4h Window 4:** avg_pnl -0.735, PF 0.454, 227 trades — fails harder
  than any other 4h window.

At similar trade counts (201 vs 227) the sign inversion is not a small-n
artifact. Interpretation: the 2h cells' tighter R:R geometry caught the
late-tape rally; the 4h cells overshot or chopped. This is useful regime
intelligence for the Tier 4 vol-regime gate (part-X carry-over #3 / part-Y
recommended item #3) — it suggests a per-timeframe regime sensitivity
that a uniform LOSS_CUT_PCT / BE_RATCHET would smear over.

## Recommended next steps (priority order)

1. **Investigate the PROVENANCE contradiction.** This is the
   highest-priority item, because it gates everything else. Find or
   reconstruct the original PROVENANCE corpus, re-run
   `XauTrendFollowBacktest --engine 4h --mode baseline` on it, and
   compare aggregate PnL to PROVENANCE's expected ~$3,296 across the
   three cited cells. If the harness reproduces PROVENANCE on the
   original corpus, the issue is tape-source-specific and the new tape
   needs characterising (volatility regime, spread distribution, gap
   density). If the harness does NOT reproduce PROVENANCE on the
   original corpus, the issue is in the harness or engine — that
   needs to be fixed before any further XTF S63 work.

2. **Patch the WF driver verdict-text** (part-X carry-over #6).
   Landed alongside this memo as a separate commit (see diffs below).
   The boilerplate "S63-adverse pattern generalises" message is
   removed and replaced with a baseline-edge-absence statement.
   Both drivers are patched (xtf_2h_wf_t1.py + xtf_4h_wf_t1.py).

3. **Create `scripts/xtf_d1_wf_t1.py`.** Mechanical "4h" → "d1" patch
   of the 4h driver, plus a per-window trade-count floor for the
   small-n D1 cadence (see `XauTrendFollowD1Engine.hpp:174-179`).
   Even though D1 is also expected to fail given the W1-W3 cross-
   timeframe consistency, completing the sibling driver set closes
   out the part-Y next-session item cluster.

4. **Defer XTF S63 sweep work.** Until #1 is resolved, running a Phase
   1 sweep over LOSS_CUT / BE_ARM / BE_BUFFER on a baseline that has
   no edge would be testing whether a protective overlay improves a
   losing strategy — which it might (S63 frequently caps losses) but
   the conclusion would not generalise to a fixed baseline.

5. **Move forward with Tier 4 Phase A on VWR USTEC.F** (part-X #3).
   Tier 4 vol-regime work is independent of XTF and remains the
   highest-leverage research track per the part-Y closing note. The
   Window 4 divergence finding adds a small piece of supporting
   evidence (per-timeframe regime sensitivity is real on XAU
   trend-follow).

## Carry-over inheritance from part-Y

| Part-Y item | Status after this session |
|---|---|
| #1 XTF 4h harness fill-in + WF | **Closed.** Harness done, WF ran, FAIL with caveats. |
| #2 XTF d1 harness fill-in + WF | Harness done (run_d1_engine path filled in part-Z S83). Driver still queued. |
| #3 Tier 4 Phase A on VWR USTEC.F | Untouched. Now highest-leverage. |
| #4 EmaPullback per-cell tuning | Untouched. |
| #5 Universe-wide S63 sweep | Untouched. ~21 state-E engines remaining. |
| #6 WF driver verdict-text patch | **Closed.** Both drivers patched alongside this memo. |
| #7 Universe-wide S63 sweep | Same as #5. |

## Run artefacts

- 4h: `outputs/xtf_4h_t1_p3_20260514_191218/`
  - `wf_summary.csv` — full per-window + AGG.
  - `wf_verdict.txt` — pre-patch verdict text (will be regenerated on
    next run with the corrected boilerplate).
  - `cells/w<N>_report.csv` + `cells/w<N>_trades.csv` — per-window
    harness outputs.
- 2h: `outputs/xtf_2h_t1_p3_20260514_191707/` — same layout.

This memo + the verdict-text driver patches close out the part-X carry-
over #1 + #6 cluster. The next session inherits the PROVENANCE-vs-WF
investigation as the top-priority XTF item, and Tier 4 Phase A on VWR
USTEC.F as the top-priority research item independent of XTF.
