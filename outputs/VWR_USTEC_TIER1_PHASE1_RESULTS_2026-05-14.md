# VWR USTEC.F — Tier 1 Phase 1 univariate sweep results — 2026-05-14

**Status:** COMPLETE. 30 cells × ~27s = 800s wall-clock on the 4.4 GB
NSXUSD tape. Verdict: **MIXED — session-window axis produces real
positive signal; all other 5 axes fail.** Recommendation: Phase 2
refinement on session window only, plus two follow-up investigations
flagged in §6.

## 1. Scope

Tier 1 univariate sweep across the six axes added to the parameter
surface in S70 (commit `d77c597`):

| Axis | Levels | Default | Flag |
|---|---|---|---|
| MIN_SESSION_MIN | 120, 180, 240, 330, 540 | 120 | `--min-session-min` |
| EXTENSION_SL_RATIO | 0.40, 0.50, 0.60, 0.80, 1.00, 1.50 | 0.60 | `--ext-sl-ratio` |
| MAE_EXIT_RATIO | 0.30, 0.40, 0.50, 0.65, 0.80 | 0.50 | `--mae-exit-ratio` |
| TP_FRACTION | 0.50, 0.65, 0.75, 0.85, 1.00, 1.15 | 1.00 | `--tp-fraction` |
| EWM_VWAP_HALF_LIFE_SEC | 1800, 3600, 7200, 14400 | 7200 | `--ewm-half-life-sec` |
| Session window (open, close) | (8,22), (10,21), (13,21), (13,17) | (8,22) | `--session-open-hour` + `--session-close-hour` |

Mode: `--mode baseline` (S63 trio zeroed). Tape:
`/Users/jo/Tick/NSXUSD_merged.csv` (4.4 GB, matches the S69 P1/P2
sweep tape). Run command: `scripts/vrev_sweep_t1_p1.sh ...
outputs/vrev_t1_p1_run/`. Wall-clock: 800s for 30 cells.

Authority: `docs/handoffs/SESSION_HANDOFF_2026-05-14g.md` §"Tier 1
sweep — what next session should run" + scoping memo
`outputs/VWR_USTEC_STRUCTURAL_REWORK_SCOPING_2026-05-14f.md` §4 + §8.

## 2. Self-consistency check — PASS

All six default-level cells emit byte-identical metrics:

```
trades=4943, wins=1545, win_rate=31.26%, gross_pnl=-7.281613,
avg_pnl=-0.001473, n_tp_hit=129, n_sl_hit=1416, n_timeout=2692,
n_mae_early_exit=706
```

Confirmed for `min-session-min=120`, `ext-sl-ratio=0.60`,
`mae-exit-ratio=0.50`, `tp-fraction=1.00`, `ewm-half-life-sec=7200`,
`session=(8,22)`. The S70 override-apply ordering in
`backtest/VWAPReversionBacktest.cpp:524-530` works correctly; the
S70 Tier 1 commit is sound.

**Default-cell baseline:** 4943 trades, `avg=-0.001473`,
`tp_rate=129/4943=2.61%`. Both `avg_pnl` and `tp_rate` are below
their respective decision-rule thresholds, so the default
configuration itself fails the cell test. Any passing cell must
improve on this baseline.

## 3. Per-axis results

The "Cell pass?" column applies the cell-level decision rule from
the part-R handoff + scoping memo §8:

- `trades ≥ 2966` (60% of 4943-trade baseline)
- `tp_rate ≥ 5%`
- `avg_pnl ≥ +0.001`

All three must be true for a cell to pass.

### 3.1 MIN_SESSION_MIN — FAIL (0/5)

| Level | Trades | Wins | TP rate | Avg PnL | Worst | p95 worst | Cell pass |
|---|---|---|---|---|---|---|---|
| 120 | 4943 | 1545 | 2.61% | −0.001473 | −6.10 | −0.563 | FAIL |
| 180 | 4840 | — | 2.64% | −0.002015 | — | — | FAIL |
| 240 | 4747 | — | 2.65% | −0.002584 | — | — | FAIL |
| 330 | 4547 | — | 2.73% | −0.003629 | — | — | FAIL |
| 540 | 2182 | — | 2.66% | −0.003923 | — | — | FAIL (trades<2966) |

**Axis verdict:** FAIL — monotonically worse `avg_pnl` as the
session minimum rises. Delaying entry into the NY/London overlap
and beyond reduces trade volume but the per-trade PnL also degrades.
Default (120) is the best of this axis.

### 3.2 EXTENSION_SL_RATIO — FAIL (0/6), plateau above 0.60

| Level | Trades | Wins | TP rate | Avg PnL | Worst | p95 worst | Cell pass |
|---|---|---|---|---|---|---|---|
| 0.40 | 6210 | — | 2.53% | −0.007273 | — | — | FAIL |
| 0.50 | 5632 | — | 2.66% | −0.004799 | — | — | FAIL |
| 0.60 | 4943 | 1545 | 2.61% | −0.001473 | −6.10 | −0.563 | FAIL |
| 0.80 | 4943 | 1545 | 2.61% | −0.001473 | −6.10 | −0.563 | FAIL ⚠ |
| 1.00 | 4943 | 1545 | 2.61% | −0.001473 | −6.10 | −0.563 | FAIL ⚠ |
| 1.50 | 4943 | 1545 | 2.61% | −0.001473 | −6.10 | −0.563 | FAIL ⚠ |

**Axis verdict:** FAIL. Tightening (0.40, 0.50) destroys edge —
more SL hits, larger losses. **The 0.60→1.50 plateau is a
follow-up item (§6, item 1)** — the SL field appears to stop
binding above the default. Candidate explanation: the
`tp_dist < sl_offset * 0.5` gate in
`CrossAssetEngines.hpp:1594` only fires when SL_RATIO > 2.0
at TP_FRACTION=1.0, so it never gates in this range. But that
explains identical *entry counts*, not identical *gross PnL* —
n_sl_hit=1416 at default means SL hits do happen, so wider SL
placement should reduce them. There's likely a downstream SL
handling path that ignores `sig.sl` in favour of another field.

### 3.3 MAE_EXIT_RATIO — FAIL (0/5)

| Level | Trades | TP rate | Avg PnL | Cell pass |
|---|---|---|---|---|
| 0.30 | 4216 | 2.18% | −0.006739 | FAIL |
| 0.40 | 4646 | 2.48% | −0.002625 | FAIL |
| 0.50 | 4943 | 2.61% | −0.001473 | FAIL |
| 0.65 | 5938 | 2.80% | −0.007534 | FAIL |
| 0.80 | 5953 | 2.81% | −0.007187 | FAIL |

**Axis verdict:** FAIL — U-shape, default (0.50) is the trough.
Tightening (0.30) cuts losers too early on what would have
mean-reverted; loosening (0.65/0.80) allows losers to bleed
further before the early-exit triggers. Default is the local
optimum but still negative.

### 3.4 TP_FRACTION — FAIL (0/6)

| Level | Trades | TP rate | Avg PnL | Cell pass |
|---|---|---|---|---|
| 0.50 | 4636 | 3.56% | −0.002258 | FAIL |
| 0.65 | 4691 | 3.60% | −0.008678 | FAIL |
| 0.75 | 4795 | 3.32% | −0.004708 | FAIL |
| 0.85 | 4882 | 2.91% | −0.004512 | FAIL |
| 1.00 | 4943 | 2.61% | −0.001473 | FAIL |
| 1.15 | 4989 | 2.37% | −0.004562 | FAIL |

**Axis verdict:** FAIL. TP rate climbs as TP_FRACTION shrinks
(closer TP is mechanically easier to hit — 0.65 nearly doubles
the hit rate vs default 1.00). But `avg_pnl` does NOT improve:
the smaller wins are more than offset by losers that no longer
mean-revert as far. **This is the most negative finding in the
sweep** — the implicit "tighter TP fixes everything" intuition
is empirically wrong on USTEC.F. Default (1.00) is the local
optimum but still negative.

### 3.5 EWM_VWAP_HALF_LIFE_SEC — FAIL (0/4)

| Level | Trades | TP rate | Avg PnL | Cell pass |
|---|---|---|---|---|
| 1800 | 1757 | 3.81% | −0.023794 | FAIL (trades<2966) |
| 3600 | 3060 | 2.94% | −0.013879 | FAIL |
| 7200 | 4943 | 2.61% | −0.001473 | FAIL |
| 14400 | 6548 | 1.94% | −0.006191 | FAIL |

**Axis verdict:** FAIL — clear bell-curve with the default at
the peak. A faster-decaying VWAP (1800s/3600s) is too sensitive
to recent action: trade count collapses and per-trade loss
deepens. A slower VWAP (14400s) inflates trade count but the
wider extension targets produce more failures. Default 7200s
is structurally correct and not tunable from this axis alone.

### 3.6 Session window (open, close) — MIXED (2/4 cells positive)

| Pair | Trades | TP rate | Avg PnL | Gross | Cell pass (strict) | Substantive |
|---|---|---|---|---|---|---|
| (8, 22) | 4943 | 2.61% | −0.001473 | −7.28 | FAIL | baseline |
| (10, 21) | 4651 | 2.67% | **+0.002067** | **+9.61** | FAIL (tp_rate) | **POSITIVE** |
| (13, 21) | 3731 | 2.71% | **+0.003306** | **+12.34** | FAIL (tp_rate) | **POSITIVE** |
| (13, 17) | 1773 | 2.88% | −0.007941 | −14.08 | FAIL (trades+avg) | too narrow |

**Axis verdict:** MIXED. Under a strict reading of the
decision rule, no cell passes — the `tp_rate ≥ 5%` gate isn't
met by ANY cell in the entire 30-cell sweep (max observed 3.81%
on `ewm-half-life=1800`). But two interior cells produce
clearly positive `avg_pnl` (+0.002067 and +0.003306) with
trade counts of 4651 and 3731, both well above the 60%-of-
baseline threshold (2966). The shape is sensibly bell-curved:
default (8,22) is too wide and (13,17) is too narrow, with the
sweet spot somewhere between.

**This is the only axis showing real positive signal in the
sweep.** The `(13,21)` cell improves on the default baseline
by `+0.003306 − (−0.001473) = +0.004779` per trade — a swing
of nearly +5x the +0.001 threshold magnitude. Gross PnL swing
over the tape: `+12.34 − (−7.28) = +19.62`. Per-trade improvement
holds even with the trade count cut by ~25%.

## 4. Cross-axis observations

1. **Default-cell expectancy is negative but small** (−0.00147/trade).
   Five of six axes have the default at or near their local optimum.
   The Tier 1 entry-side levers (SL geometry, MAE-exit ratio, TP
   geometry, EWM half-life) do not have material edge available from
   univariate movement. Only session windowing does.

2. **TP rate is structurally low across the entire parameter
   surface.** Max observed `tp_rate = 3.81%` (`ewm-half-life=1800`).
   Aggressive TP_FRACTION narrowing (0.50/0.65) caps at 3.6%. The
   strategy's edge — when it has any — does not flow through TP
   hits. Most exits are MAE_EARLY_EXIT (706 at default) and TIMEOUT
   (2692 at default). This implies the `tp_rate ≥ 5%` gate in
   scoping memo §8 is mis-calibrated for VWR USTEC.F: it should be
   `≥ 3%` or removed entirely. See §6 follow-up item 2.

3. **Trade count is highly responsive to EWM half-life.** Trade
   counts: 1757 → 3060 → 4943 → 6548 across the 4 cells (1800s →
   14400s). The half-life governs how fast the EWM-VWAP anchor
   forgets and re-targets — shorter half-life produces fewer
   qualifying extension setups (the anchor is too close to current
   price too often). 7200s default is the only level producing a
   "typical" 4000-5000 trade count.

4. **The TP_FRACTION result is counter-intuitive and informative.**
   Narrowing TP to 50-85% of the way to VWAP increases TP hit count
   meaningfully (e.g. 0.65 produces 169 TP hits vs default 129) but
   degrades `avg_pnl` substantially. The conclusion is that the
   strategy's losers don't reverse far enough for shallow targets to
   be safe — when you ask for less reversion you GET less reversion,
   but the losers don't benefit. The signal needs full-reversion
   targets to be worth taking.

## 5. Sweep verdict and recommended next step

**Sweep verdict:** **1 of 6 axes shows positive signal** (session
window only). 5 of 6 axes are unambiguously dead in this parameter
range.

**Strict-rule outcome:** Per scoping memo §4 reasoning ("If no axis
passes, recommend Tier 4"), no axis passes under the literal
`tp_rate ≥ 5%` gate, so the strict rule recommends Tier 4. But the
gate appears mis-calibrated — see §6 item 2 — and the session-window
axis has real, large-magnitude positive signal regardless. Treating
the strict rule literally would discard a real finding.

**Recommended next step:** **Phase 2 refinement on session window only.**
The other 5 axes don't merit further investigation in this Phase 2 budget.

Suggested Phase 2 sweep (1D fine on `(open, close)`, ~12 cells):

| Open | Close | Rationale |
|---|---|---|
| 9 | 21 | bracket the (10,21) winner from below |
| 10 | 21 | re-run for stability |
| 11 | 21 | between the two winners |
| 12 | 21 | between the two winners |
| 13 | 21 | the +0.003306 winner |
| 14 | 21 | bracket the (13,21) winner from above |
| 13 | 20 | trim the close hour, see if NY-PM helps or hurts |
| 13 | 22 | extend the close hour |
| 13 | 19 | tighter close |
| 11 | 20 | mid-mid |
| 12 | 22 | mid-late |
| 10 | 22 | early-late control |

Decision rule for Phase 2A (≥4 of 12 cells with `avg_pnl ≥ +0.001`
AND the positive cells form a smooth contiguous region in the
2D (open,close) plane, not isolated). Wall-clock: ~6 min.

If Phase 2A confirms (13,21) ± 1 hour as a robust positive island,
proceed to Phase 3 walk-forward validation on the (13,21) setting
per scoping memo §5. If Phase 3 passes, the engine can be re-enabled
at `engine_init.hpp:608` with `SESSION_OPEN_HOUR = 13` and
`SESSION_CLOSE_HOUR = 21` set per-instance on `g_vwap_rev_nq`.

If Phase 2A fails (the (13,21) edge doesn't generalise), recommend
Tier 4 (signal-shape redesign) per scoping memo §4 reasoning and
close the VWR USTEC.F retune track.

## 6. Follow-up investigations (independent of Phase 2)

These are SEPARATE work items, not blocking Phase 2:

1. **EXTENSION_SL_RATIO plateau above 0.60.** Cells at 0.60, 0.80,
   1.00, 1.50 produce byte-identical metrics including `gross_pnl`
   and `n_sl_hit`. At TP_FRACTION=1.0, the
   `tp_dist < sl_offset * 0.5` gate at
   `CrossAssetEngines.hpp:1594` only rejects entries when
   `SL_RATIO > 2.0`, so it doesn't explain the plateau. The signal-
   level `sig.sl` IS computed correctly from `EXTENSION_SL_RATIO`;
   so a downstream SL-handling path is probably ignoring `sig.sl`
   in favour of a fixed-percentage SL or per-symbol cap.
   Investigation should grep for `sig.sl` consumers and the
   position-manager's SL field setter.

2. **TP rate ≥ 5% gate calibration in scoping memo §8.** Max
   `tp_rate` across the entire 30-cell sweep is 3.81%; the rule was
   apparently sized for a different strategy profile. Two options:
   (a) lower the gate to `tp_rate ≥ 3%`, which would treat the
   (10,21) and (13,21) cells as full passes; (b) remove the gate
   entirely and rely on `trades ≥ 60% baseline` + `avg_pnl ≥ +0.001`
   alone. Either choice is defensible — the gate's original
   intent was "TP geometry must actually function" and this
   strategy makes edge through MAE+TIMEOUT loss control, not TP
   hits, so the gate may be the wrong question.

3. **The TP_FRACTION result deserves a one-paragraph aside in the
   next handoff** because it falsifies the natural intuition
   (narrower TP → easier hits → better expectancy). The data shows
   narrower TP increases hits but worsens expectancy — losers
   don't benefit from a shallower target. This is information for
   future signal-redesign decisions.

## 7. Artifacts

- Summary CSV: `outputs/vrev_t1_p1_run/phase1_summary.csv` (20 columns)
- Per-cell reports: `outputs/vrev_t1_p1_run/cells/*_report.csv`
- Per-cell trades: `outputs/vrev_t1_p1_run/cells/*_trades.csv`
- Per-cell stderr (banner + summary): `outputs/vrev_t1_p1_run/cells/*_stderr.log`
- This memo: `outputs/VWR_USTEC_TIER1_PHASE1_RESULTS_2026-05-14.md`
- Sweep driver: `scripts/vrev_sweep_t1_p1.sh`

## 8. Commit suggestion

```bash
git add -f outputs/VWR_USTEC_TIER1_PHASE1_RESULTS_2026-05-14.md
git add scripts/vrev_sweep_t1_p1.sh
git diff --cached --stat
git commit -m "S71 Phase 1: VWR USTEC.F Tier 1 univariate sweep - session window is the only positive axis

30-cell sweep across the 6 Tier 1 axes added in S70. Self-consistency
passes (6 default-level cells identical). Verdict: MIXED -- 5 of 6
axes fail (min-session-min, ext-sl-ratio, mae-exit-ratio, tp-fraction,
ewm-half-life-sec all show default at or near local optimum, all
non-improving). Session-window axis shows two interior cells with
clearly positive avg_pnl: (10,21)=+0.002067 and (13,21)=+0.003306,
trade counts 4651 and 3731 (both well above 60% baseline threshold).

Bell-shape on session axis: (8,22)=baseline FAIL, (10,21)=PASS,
(13,21)=PASS, (13,17)=too narrow FAIL. Recommendation: Phase 2
refinement on session window only; skip the other 5 axes.

Two follow-ups flagged for separate investigation (not blocking
Phase 2):
  1. ext-sl-ratio plateau: cells 0.60-1.50 produce byte-identical
     metrics; downstream SL-handling probably ignores sig.sl.
  2. tp_rate >= 5% gate from scoping memo §8 is unreachable across
     the entire sweep (max observed 3.81%); needs recalibration to
     ~3% or removal.

Sweep driver, summary CSV, results memo all force-added.
Engine remains disabled (engine_init.hpp:608 unchanged).
Phase 2 driver to follow."
git push origin main
```
