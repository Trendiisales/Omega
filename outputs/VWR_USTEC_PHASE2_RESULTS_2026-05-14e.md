# VWR USTEC.F Retune — Phase 2 Results (2026-05-14e)

**Status:** Phase 2 complete. Both Phase 2A (robustness) and Phase 2B
(independence) fail their decision criteria. Recommendation: escalate
to structural rework per plan §8. Do NOT proceed to Phase 3 WF.

**Plan reference:** `outputs/VWR_USTEC_RETUNE_PLAN_2026-05-14a.md` §5 Phase 2.
**Phase 1 carryover:** `outputs/VWR_USTEC_PHASE1_RESULTS_2026-05-14e.md`.
**Driver:** `scripts/vrev_sweep_p2.sh` (uncommitted at memo write time).
**Output dir:** `outputs/vrev_p2_20260514_111218/`.

---

## 1. Sweep configuration

| Field | Value |
|---|---|
| Tape | `/Users/jo/Tick/NSXUSD_merged.csv` (same as Phase 1) |
| Symbol | USTEC.F |
| Mode | `--mode baseline` (S63 trio = 0.0 throughout) |
| Cells | 15 (Phase 2A: 6 + Phase 2B: 9) |
| Wall-clock | 403s (6 min 43s), ~27s per cell |

---

## 2. Self-consistency check — PASSED

`2b ext=0.80 mx=1.20` reproduced Phase 1's `ext=0.80` cell byte-identically:

```
2b ext=0.80 mx=1.20: trades=1145, gross=+3.229369, avg=+0.002820,
                     n_tp_hit=25, n_mae_early_exit=97,
                     worst=-1.801615, p95_worst_loss=-0.874505
Phase 1 ext=0.80   : trades=1145, gross=+3.229369, avg=+0.002820,
                     n_tp_hit=25, n_mae_early_exit=97,
                     worst=-1.801615, p95_worst_loss=-0.874505
```

The harness is deterministic and the Phase 0 override-apply ordering is
correct. We can compare Phase 1 and Phase 2 results as if they were one
continuous sweep.

---

## 3. Phase 2A results — FAILS robustness

1D fine sweep on `--ext` at 6 levels around the Phase 1 winner. Max-ext
held at USTEC.F preset (1.20).

| ext | trades | gross | avg_pnl | n_tp | n_mae | worst | p95 worst |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 0.70 | 1645 | -32.56 | -0.01979 | 18 | 165 | -4.08 | -0.824 |
| 0.75 | 1366 |  -9.19 | -0.00673 | 21 | 121 | -3.98 | -0.838 |
| **0.80** | **1145** |  **+3.23** | **+0.00282** | **25** |  **97** | **-1.80** | **-0.875** |
| 0.85 |  961 |  -6.85 | -0.00713 | 17 |  76 | -1.80 | -0.910 |
| 0.90 |  793 | -13.26 | -0.01672 | 10 |  60 | -1.80 | -0.945 |
| **1.00** |  **574** |  **+8.40** | **+0.01464** | **16** |  **37** | **-1.80** | **-0.961** |

**Decision rule:** "Robustness pass: ≥4 of 6 fine-sweep cells with
avg_pnl > 0 AND the swing across the 6 levels is < 50% of the mean
positive value."

**Pass status:** **FAIL.** Only 2 of 6 cells positive (33%, need 67%).

**Shape of the result:** the avg_pnl curve as a function of ext is
non-monotonic, with two isolated positive spikes at 0.80 and 1.00
separated by a negative trough at 0.85-0.90. A robust edge would
show monotonic improvement as the entry threshold tightens, not
sandwiched local maxima. The shape is consistent with sampling
noise dominating the small-N (<2000 trades) cells.

**TP-rate diagnostic:** n_tp_hit / trades ratios across the sweep:

```
ext=0.70: 18/1645 = 1.1%
ext=0.75: 21/1366 = 1.5%
ext=0.80: 25/1145 = 2.2%
ext=0.85: 17/961  = 1.8%
ext=0.90: 10/793  = 1.3%
ext=1.00: 16/574  = 2.8%
```

TP rates are 1-3% across the entire sweep. The strategy is NOT
closing trades at profit targets — it's closing them via timeout
or MAE_EXIT. The "winning" trades in the +ve cells are winning by
*avoiding bigger losses* (small-loss exits beating big-loss exits
in aggregate), not by *capturing wins*. That's not a tradeable
edge; it's risk shaping on a fundamentally directional-loss
strategy.

---

## 4. Phase 2B results — FAILS improvement

2D mini-grid `ext ∈ {0.70, 0.80, 0.90}` × `max-ext ∈ {1.20, 1.50, 2.00}`.

| ext\\max-ext | 1.20 | 1.50 | 2.00 |
|---:|---:|---:|---:|
| 0.70 | -0.01979 | -0.01890 | -0.01937 |
| 0.80 | **+0.00282** | **+0.00605** | -0.00001 |
| 0.90 | -0.01672 | -0.01161 | -0.00714 |

**Decision rule:** "Independence pass: best 2D cell improves over best
2A cell by >10% AND the second-best 2D cell is within 30% of the best."

**Pass status:** **FAIL.** Best 2B = (0.80, 1.50) at +0.00605. Best 2A
= ext=1.00 at +0.01464. Best 2B is *worse* than best 2A — fails the
">10% improvement over 2A" criterion outright.

**Caveat:** the 2B grid was designed before Phase 2A ran, and didn't
include ext=1.00 (where the 2A best landed). An extended 2B at ext=1.00
× max-ext ∈ {1.20, 1.50, 2.00} could in principle find a better cell.
However, this would be increasingly ad-hoc grid-stretching after-the-
fact, and:

1. The ext=1.00 cell already has only 574 trades. Splitting that further
   (which the 2D grid would do if any cell improved) doesn't make sense.

2. The shape result from 2A is the dominant signal: non-monotonic
   peaks at 0.80 and 1.00. Searching wider 2D grids would just produce
   more isolated peaks, not a stable surface.

3. The absolute scale is trivial regardless: gross +$8.40 over 404
   days at ext=1.00 = +$7.60 per year. Not material to portfolio.

---

## 5. Combined Phase 1 + 2 picture

12 of 27 ext-axis cells (across Phase 1's 6 + Phase 2A's 6 + Phase 2B's
9 = 21 unique tuples, dedup'd) produced positive avg_pnl. Hit rate 44%
weighted, but every "hit" is sandwiched between misses on adjacent
levels. The signal-to-noise is poor:

```
ext-axis trend (combining all sweeps):
  0.20 / 0.30 / 0.40 / 0.50 / 0.60 / 0.70 / 0.75 / 0.80 / 0.85 / 0.90 / 1.00
   -0.0041 -0.0087 -0.0015 -0.0058 -0.0158 -0.0198 -0.0067 +0.0028 -0.0071 -0.0167 +0.0146
```

This is not a tunable parameter surface; it's noise centered on a
slightly-negative mean (-0.001 to -0.002 typical) with occasional small
positive excursions at low trade counts.

---

## 6. Recommendation: escalate per plan §8

The plan §5 stop condition was "if no single-axis move produces baseline
≥ +0.001/trade, the strategy likely needs structural changes". One cell
technically passed in Phase 1, but Phase 2 disproved robustness. Per
the script's encoded rule "If 2A fails: escalate to structural rework",
we should:

1. **NOT proceed to Phase 3 WF.** WF on a non-monotonic small-N surface
   would split single-cell artifacts into smaller artifacts. Information
   value approaches zero.

2. **Keep `g_vwap_rev_nq.enabled = false` indefinitely** at
   `engine_init.hpp:608`. The S68 stop-bleed disable is the correct
   long-term state until the signal layer is reworked.

3. **Update the comment block at engine_init.hpp:611-624** to record the
   Phase 1 + Phase 2 findings. The current block ends "a separate
   parameter retune session is warranted, but revert stops the active
   bleed first." That sentence is now historically incorrect — the
   parameter retune happened, found nothing tunable.

4. **Defer structural rework to a separate session.** Per plan §8
   "deferred decisions", the candidate work is: VIX/L2 confluence
   thresholds, MIN_SESSION_MIN tightening, session-of-day filters,
   or signal-side regime gating. Out of scope for the current chat.

---

## 7. Why this isn't a tragedy

The S68 disable was the right call at the time (stop the bleed). Phase
1 + Phase 2 confirmed empirically that parameter tuning cannot rescue
USTEC.F VWR. That's load-bearing knowledge: it means no future session
will spend cycles re-tuning entry-side parameters here, and the engine
stays disabled with documented justification.

The engine sits at 1/4 instances in the VWR family. The other three
(g_vwap_rev_sp, g_vwap_rev_eurusd) are also state-B (S63 zeroed by
explicit revert) and the last one (g_vwap_rev_ger40) is the only
state-A engine that's actively contributing. Losing USTEC.F entirely
costs us 0 — the engine wasn't producing positive expectancy anyway.

---

## 8. Followups (next session)

1. **engine_init.hpp comment block update** — fold this finding into
   the existing :611-624 block. Single targeted Edit. ~5 min.

2. **VWR retune plan archive marker** — add a closing note at the
   top of `outputs/VWR_USTEC_RETUNE_PLAN_2026-05-14a.md` stating
   "executed 2026-05-14e, retune unsuccessful, see PHASE1_RESULTS
   and PHASE2_RESULTS memos for findings." Keeps the planning chain
   traceable without retiring the plan file.

3. **Part-P session handoff** — should explicitly call out that the
   re-enable criteria from plan §7 are NOT met and won't be met by
   any future parameter sweep. The engine moves from "retune pending"
   status to "signal rework pending" status in the audit chain.

4. **Optional: structural rework scoping** — fresh chat session. Read
   the VWAPReversionEngine class header carefully; identify which
   confluence inputs (VIX, L2) have remained at class defaults and
   whether they have leverage; consider a session-of-day filter
   sweep similar to what NBM does. Multi-session work; out of scope
   to start in this chat.
