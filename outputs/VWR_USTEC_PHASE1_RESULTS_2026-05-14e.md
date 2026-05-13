# VWR USTEC.F Retune — Phase 1 Results (2026-05-14e)

**Status:** Phase 1 complete. One cell passes the avg_pnl ≥ +0.001 threshold.
Decision pending on whether to proceed to Phase 2 (2D fine sweep) or treat
this as a signal-level rework finding.

**Plan reference:** `outputs/VWR_USTEC_RETUNE_PLAN_2026-05-14a.md` §5 Phase 1.
**Driver:** `scripts/vrev_sweep_p1.sh` (commit S69 P1 when committed).
**Harness:** `backtest/VWAPReversionBacktest.cpp` with S69 P0 CLI extensions
(commit `e625b26`).

---

## 1. Sweep configuration

| Field | Value |
|---|---|
| Tape | `/Users/jo/Tick/NSXUSD_merged.csv` |
| Tape size | 4.4 GB, 125,060,055 data rows (after 1 header row) |
| Tape format | `ts_ms,bid,ask` (format A per harness auto-detect) |
| Symbol | USTEC.F |
| Mode | `--mode baseline` (S63 trio = 0.0 throughout) |
| Cells | 21 univariate (6 + 5 + 5 + 5) |
| Wall-clock | 557s (9 min 17s), ~26-28s per cell |
| Output dir | `outputs/vrev_p1_20260514_105824/` |

---

## 2. Self-consistency check — PASSED

The four cells that hit the per-symbol preset for their swept axis
(`ext=0.40`, `max-ext=1.20`, `max-hold=600`, `cooldown=300`) are
byte-identical:

```
ext=0.40       : ext,0.40,4943,-7.281613,-0.001473,129,706,-6.104785,-0.562875
max-ext=1.20   : max-ext,1.20,4943,-7.281613,-0.001473,129,706,-6.104785,-0.562875
max-hold=600   : max-hold,600,4943,-7.281613,-0.001473,129,706,-6.104785,-0.562875
cooldown=300   : cooldown,300,4943,-7.281613,-0.001473,129,706,-6.104785,-0.562875
```

The shared baseline (-0.001473 / trade) matches the part-L documented
USTEC.F baseline expectancy of -0.00147/trade exactly (see
`include/engine_init.hpp:622`). Harness wiring is correct end-to-end.

---

## 3. Per-cell results

| Axis | Level | Trades | Gross | Avg/trade | n_tp | n_mae | Worst | p95 worst |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| ext | 0.20 | 10952 | -44.36 | -0.00405 | 302 | 2378 | -8.41 | -0.444 |
| ext | 0.30 |  7250 | -63.02 | -0.00869 | 179 | 1273 | -8.64 | -0.500 |
| ext | 0.40 |  4943 | -7.28  | -0.00147 | 129 |  706 | -6.10 | -0.563 |
| ext | 0.50 |  3334 | -19.29 | -0.00579 |  94 |  413 | -5.20 | -0.650 |
| ext | 0.60 |  2354 | -37.19 | -0.01580 |  53 |  245 | -5.03 | -0.758 |
| **ext** | **0.80** |  **1145** | **+3.23**  | **+0.00282** |  **25** |   **97** | **-1.80** | **-0.875** |
| max-ext | 0.80 |  4773 | -12.72 | -0.00266 | 130 |  707 | -6.10 | -0.551 |
| max-ext | 1.00 |  4875 | -20.34 | -0.00417 | 126 |  715 | -6.10 | -0.567 |
| max-ext | 1.20 |  4943 | -7.28  | -0.00147 | 129 |  706 | -6.10 | -0.563 |
| max-ext | 1.50 |  4935 | +0.41  | +0.00008 | 129 |  711 | -6.10 | -0.563 |
| max-ext | 2.00 |  4946 | +0.43  | +0.00009 | 129 |  707 | -6.10 | -0.561 |
| max-hold | 300  | 7508 | -10.13 | -0.00135 | 146 |  504 | -8.41 | -0.492 |
| max-hold | 600  | 4943 | -7.28  | -0.00147 | 129 |  706 | -6.10 | -0.563 |
| max-hold | 900  | 3987 | -16.33 | -0.00410 | 119 |  797 | -6.10 | -0.631 |
| max-hold | 1200 | 3483 | -24.78 | -0.00711 | 120 |  868 | -6.10 | -0.709 |
| max-hold | 1500 | 3209 | -7.29  | -0.00227 | 124 |  911 | -6.10 | -0.764 |
| cooldown | 120 | 4957 | -6.37  | -0.00129 | 133 |  722 | -6.10 | -0.559 |
| cooldown | 180 | 4948 | -12.45 | -0.00252 | 133 |  722 | -6.10 | -0.561 |
| cooldown | 300 | 4943 | -7.28  | -0.00147 | 129 |  706 | -6.10 | -0.563 |
| cooldown | 600 | 4869 | -11.15 | -0.00229 | 122 |  676 | -6.10 | -0.567 |
| cooldown | 900 | 4131 | -18.38 | -0.00445 | 109 |  592 | -6.10 | -0.583 |

Bold row = only cell passing the avg_pnl ≥ +0.001 decision threshold.

---

## 4. Reading the result

**The strategy works marginally only when entry is very restrictive.**
Only `ext=0.80` (twice the current USTEC override, four times the class
default) produces net-positive expectancy. The mechanism is mostly
trade-count reduction: 1145 trades vs 4943 baseline (-77%), TP rate
collapses to 2.2% (25/1145), and the catastrophic worst-trade tail
(-6.10 in every other cell) disappears (-1.80 at ext=0.80) — but
that's the absolute worst trade, not the typical bad trade. p95
worst loss actually *worsens* at ext=0.80 (-0.875 vs -0.563 baseline),
meaning the trades that do happen at ext=0.80 are larger on average,
both winners and losers. The net is positive but small: +$3.23 gross
over 404 days, vs -$7.28 baseline → +$10.51 net improvement.

**All other axes failed to improve baseline meaningfully.** max-ext
1.50/2.00 are marginally positive (+0.000083 / +0.000087) but well
below the +0.001 threshold and would be a one-side-of-the-coin
selection (max-ext only loosens the upper cap; the engine still trades
the same trigger frequency). max-hold and cooldown both have the
preset as the local optimum — every shift away from it makes the
result worse.

**Implication:** the VWR USTEC.F entry signal is generating mostly
noise. Filtering more aggressively at the entry threshold *can*
produce positive expectancy but at the cost of trade volume that
makes the engine's contribution to portfolio P&L negligible. Other
levers don't help.

---

## 5. Decision: three options

### Option A — Proceed to Phase 2 around ext=0.80 (per plan §5)

1D fine sweep on `--ext` at levels {0.70, 0.75, 0.80, 0.85, 0.90, 1.00}
to check whether the +0.002820 result is robust or a single-cell
artifact. If robust → 2D refinement with the marginal max-ext axis
({1.50, 2.00} × ext top-3) to look for independence. ~30 min wall-clock.

Pros: cheapest path. If ext=0.80 survives, we have a tradeable
result and can move to Phase 3 (walk-forward validation).

Cons: even in the best case the engine would contribute ~$10/yr per
4943-trade equivalent — i.e., basically nothing. Not clear the edge
is worth productionising over an engine that's currently disabled.

### Option B — Declare structural rework needed (per plan §5 stop condition spirit)

The stop condition is "no single-axis move produces baseline ≥
+0.001 / trade". One cell passing on ext at the cost of 77% trade
volume is arguably a degenerate pass — the strategy isn't being
fixed, it's being shut off. Per the plan §8 deferred-decisions
section, the next path is signal-side: VIX/L2 confluence thresholds,
MIN_SESSION_MIN tightening, or session-of-day filters.

Pros: avoids chasing a marginal edge that may not survive WF. Frees
up session time for higher-leverage work (P4 wrapper engine S63
audit, etc.).

Cons: structural rework is multi-session and harder to scope. The
engine stays disabled longer.

### Option C — Compromise: Phase 2 first, structural rework if WF fails

Run Phase 2 (~30 min, cheap). If ext=0.80 survives, run Phase 3 WF
(~45 min). If WF passes, re-enable. If WF fails, escalate to
structural rework.

Pros: uses cheap exploration to gate the more expensive decision.
Doesn't prematurely abandon a passing cell.

Cons: same as Option A's cons but with a fallback. Total time
investment is up to ~2 hours.

---

## 6. Recommendation

Option C. The cost of Phase 2 is small (~30 min unattended), the
information value is real (does the +0.002820 hold across
neighbouring levels?), and the decision rule is clean (if WF
fails, we escalate). Skipping straight to structural rework based on
one marginal pass would be over-fitting our skepticism to a single
data point.

---

## 7. Followups regardless of A/B/C choice

- **Engine_init comment update.** When this retune concludes (whether
  with re-enable or with a structural-rework escalation), the comment
  block at `engine_init.hpp:611-624` should be extended with the
  Phase 1 finding so the next session inherits the context. Currently
  that block stops at "a separate parameter retune session is
  warranted, but revert stops the active bleed first."

- **Results CSV in outputs/.** `outputs/vrev_p1_20260514_105824/phase1_summary.csv`
  is the canonical Phase 1 artifact. Gitignored (under outputs/), so
  preserve manually or fold the table into this memo (done above)
  for the audit trail.

- **Self-consistency invariant.** The four-cells-match check should
  be a permanent fixture of any future VWR sweep driver — it caught
  zero bugs this time (good) but it's the cheap check that catches
  the override-apply-order bug if it ever reappears.
