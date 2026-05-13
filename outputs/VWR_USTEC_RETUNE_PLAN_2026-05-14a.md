# VWAPReversion USTEC.F — Parameter Retune Plan (2026-05-14a)

**Status:** planning memo, no code changes (other than the small harness
extension described in Phase 0).
**Author session:** part 2026-05-14a, following SESSION_HANDOFF_2026-05-14a.md.
**Trigger:** part-K/L queued follow-up. `g_vwap_rev_nq.enabled = false`
(S68 stop-bleed); re-enable is gated on this retune producing a
positive-expectancy parameter set on fresh tape.

---

## 1. Why this exists

Three pieces of evidence converge:

(a) **S37-H S63 cuts make USTEC.F worse.** Part-L smoke test on
NSXUSD 4943-trade tape: TP_HIT count 129 → 58 (-55% winner amputation),
gross_pnl -7.28 → -8.05 (already-losing strategy got worse). Worst-trade
got worse (-6.10 → -8.40) even though p95_loss improved
(-0.56 → -0.21). The cuts work for GER40 (fat-tailed) but amputate
winners on USTEC.F. See `engine_init.hpp:611-624` for the part-L
revert commentary.

(b) **USTEC.F VWR baseline is marginally negative.** The same 4943-trade
sweep showed baseline expectancy of -0.00147 / trade. The strategy
itself needs work, separate from any in-flight protection layer.

(c) **The S68 disable is a band-aid, not a fix.** `g_vwap_rev_nq.enabled
= false` stops the bleed but parks an entire signal source. The
intended path forward (part-L handoff item 3) is to retune the entry-
side parameters, validate baseline → positive on fresh tape, then
re-enable.

This memo plans the retune. It does NOT execute it — sweep wall-clock
plus walk-forward validation needs its own session (estimated 2-3 hrs).

---

## 2. Parameter surface

The class lives at `include/CrossAssetEngines.hpp:1202-1247`. The
USTEC.F-relevant tunable surface is:

| Param | Current (NQ override) | Class default | Sweep candidate? |
|---|---|---|---|
| `EXTENSION_THRESH_PCT` | 0.40 | 0.20 | **YES** — entry gate |
| `EXTENSION_SL_RATIO` | (default) 0.60 | 0.60 | Maybe — affects SL distance |
| `MAE_EXIT_RATIO` | (default) 0.50 | 0.50 | Maybe — legacy safety net |
| `MAX_EXTENSION_PCT` | 1.20 | 0.80 | **YES** — entry cap |
| `MAX_HOLD_SEC` | 600 | 900 | **YES** — timeout |
| `COOLDOWN_SEC` | 300 | 180 | **YES** — entry frequency |
| `MAE_COOLDOWN_SEC` | (default) 600 | 600 | No — post-loss only |
| `CONSEC_FC_BLOCK_SEC` | (default) 1800 | 1800 | No — protective block |
| `TP_FLIP_COOLDOWN_SEC` | (default) 1200 | 1200 | No — direction block |
| `MIN_SESSION_MIN` | (default) 120 | 120 | No — session warmup |
| `LOSS_CUT_PCT` | 0.0 | 0.08 (class) | **YES** — but downstream |
| `BE_ARM_PCT` | 0.0 | 0.05 | **YES** — but downstream |
| `BE_BUFFER_PCT` | 0.0 | 0.02 | **YES** — but downstream |
| `CONF_VIX_THRESH` | (default) 18.0 | 18.0 | No — VIX confluence |
| `CONF_L2_THRESH` | (default) 0.12 | 0.12 | No — L2 confluence |
| `EWM_VWAP_HALF_LIFE_SEC` | (static constexpr) 7200 | n/a | No — structural |

"Downstream" = the S63 trio is evaluated AFTER baseline expectancy
turns positive. There is no point sweeping a giveback-prevention layer
on a strategy whose baseline is losing money.

---

## 3. Harness extension (Phase 0)

`backtest/VWAPReversionBacktest.cpp` currently exposes only the S63 trio
to CLI override (`--loss-cut` / `--be-arm` / `--be-buffer`). The entry-
side params live in `params_for(symbol)` presets (~L237-244 for NQ),
which means sweeping them today requires per-cell recompile.

Add these CLI flags (~30 LOC change, no engine touch):

```
--ext <pct>        override EXTENSION_THRESH_PCT
--max-ext <pct>    override MAX_EXTENSION_PCT
--max-hold <sec>   override MAX_HOLD_SEC
--cooldown <sec>   override COOLDOWN_SEC
```

Pattern is mechanical — mirror the existing `--loss-cut` flag wiring at
L389/398/425, then apply at L464-468 where the engine fields are
assigned from `p`. Banner at L447-453 also needs the new params printed.

Test: build, then `./build/VWAPReversionBacktest <tape> --symbol USTEC.F
--ext 0.30 --quiet` should report `EXTENSION_THRESH_PCT = 0.3000  (cli
override)` in the banner.

**Status:** not coded yet. This is the unblocking step for any actual
sweep work. ~15 min implementation.

---

## 4. Tape

The part-K/L sweep used NSXUSD HistData (4943 trades on 404 days =
roughly Aug 2024 → Sept 2025). Two issues with re-using it as-is:

(a) **Tape staleness.** It's now 7-9 months old at the trailing edge.
Volatility regime has likely shifted; the sweep result risks being
calibrated to a regime that no longer applies.

(b) **Symbol alias mismatch.** NSXUSD is the Dukascopy symbol for
NAS100/USTEC. The harness's `params_for("USTEC.F")` preset is correct,
but reading needs the right loader path.

**Recommended primary tape:** the same `XAUUSD_2024-03_2026-04_combined.csv`
locator pattern (Dukascopy combined CSV, ~25-month window) applied to
NSXUSD. If that file doesn't exist, the next session should:

1. Pull NSXUSD from Dukascopy via the existing helper (whatever was
   used to produce the XAUUSD file).
2. Convert with `scripts/duka_to_legacy.py` (the part-K converter) if
   needed for harness format compatibility.

**Backup tape:** the existing 404-day NSXUSD HistData file (location
referenced in part-K handoff outputs). Older but functional; sufficient
for a first-pass sweep if the fresh Dukascopy pull isn't worth the
overhead.

---

## 5. Sweep plan

Three phases. Each phase's output gates the next.

### Phase 1 — Univariate baseline edge probe

Goal: find whether ANY entry-side parameter change moves baseline
expectancy off the marginal-negative value (-0.00147 / trade).

Fix `LOSS_CUT_PCT/BE_ARM_PCT/BE_BUFFER_PCT = 0.0` throughout this phase
(via `--mode baseline`). Vary one entry param at a time:

| Param | Levels | Rationale |
|---|---|---|
| `--ext` | 0.20, 0.30, 0.40, 0.50, 0.60, 0.80 | Class default 0.20 → tighter; current 0.40 → looser |
| `--max-ext` | 0.80, 1.00, 1.20, 1.50, 2.00 | Class default 0.80; current 1.20 |
| `--max-hold` | 300, 600, 900, 1200, 1500 | Class default 900; current 600 |
| `--cooldown` | 120, 180, 300, 600, 900 | Class default 180; current 300 |

Total: 6+5+5+5 = **21 univariate runs**.

Wall-clock per run: estimated ~30-90s for 4943 trades through 404 days,
plus tick replay. So ~30 min total for Phase 1 if quiet-mode.

Output: 21-row CSV with (param, level, gross_pnl, tp_hit, mae_exit,
worst_trade, p95_loss, trades_count). Look for any 1D move that produces
positive baseline.

**Stop condition:** if no single-axis move produces baseline ≥ +0.001 /
trade, the strategy likely needs structural changes (entry signal,
TP/SL geometry, session timing) rather than parameter tuning. Memo
that finding and stop.

### Phase 2 — 2D refinement around any Phase-1 winner

For each axis where Phase 1 showed >25% improvement vs current, do a
2D fine sweep at the top-2 levels of that axis × the next axis to
check independence:

- e.g. if `--ext = 0.30` and `--max-ext = 1.00` both improved → 4-cell
  fine grid (ext ∈ {0.25, 0.30, 0.35} × max-ext ∈ {0.90, 1.00, 1.10}).
- if Phase-1 winners are `--ext` and `--cooldown` → similar 9-cell
  grid.

Total: typically 9-16 cells, ~10-25 min.

### Phase 3 — Walk-forward validation

CRITICAL: do not commit a winner from Phase 2 without WF. The 4943-
trade tape has enough run-up that single-tape best-fit will overfit.

Approach: split tape into 3 contiguous folds (early/mid/late). For each
fold, hold out as OOS:

1. Train on fold A+B, OOS-test on fold C → record OOS gross_pnl
2. Train on fold A+C, OOS-test on fold B
3. Train on fold B+C, OOS-test on fold A

A parameter set is "validated" only if all three OOS gross_pnl are
positive AND the std of OOS expectancy is < 50% of mean expectancy.

If a set passes WF: candidate for `g_vwap_rev_nq` re-enable.

---

## 6. Re-evaluating S63 post-retune

ONLY after Phase 3 produces a positive-expectancy baseline.

Then sweep the S63 trio on top of the new baseline:

| Param | Levels |
|---|---|
| `--loss-cut` | 0.0, 0.05, 0.08, 0.12, 0.16 |
| `--be-arm` | 0.0, 0.03, 0.05, 0.08 |
| `--be-buffer` | 0.01, 0.02, 0.03 |

Skip cells where loss-cut == 0 AND be-arm == 0 (baseline already known).
Skip cells where be-arm > 0 AND be-buffer == 0 (degenerate).

Practical sub-grid: ~40 cells. ~30 min.

Decision rule: keep S63 enabled only if it improves expectancy AND
worst-trade ratio simultaneously. The part-L failure mode was worst-
trade getting worse despite p95 improving; do not accept that trade-off
again.

If no S63 cell beats baseline-only: leave `LOSS_CUT_PCT/BE_ARM_PCT/
BE_BUFFER_PCT = 0.0` in engine_init.hpp with documenting comment
mirroring the existing part-K/L precedent at engine_init.hpp:597-624.

---

## 7. Re-enable criteria

`g_vwap_rev_nq.enabled = true` is appropriate when ALL of:

1. Phase 3 WF passes (3-fold OOS, all positive, std < 50% of mean).
2. Expected drawdown from the WF folds is ≤ the operator's per-engine
   risk tolerance (memo doesn't define this — confirm with operator
   at re-enable time).
3. The corresponding engine_init.hpp settings change is committed in
   the same commit as a results CSV in `outputs/`.
4. The S37-P2 RiskMonitor thresholds row for VWAPReversion USTEC.F
   (in `data/risk_monitor_thresholds.csv`) is reviewed and updated if
   the new fire rate differs materially from the historical baseline.

---

## 8. Out-of-scope (deferred decisions)

- **Signal change.** If Phase 1 univariate sweep finds no positive
  baseline, the path forward is signal-side: VIX/L2 confluence
  thresholds, MIN_SESSION_MIN tightening, or session-of-day filters.
  Those are structural and warrant a separate design pass.

- **`EXTENSION_SL_RATIO` and `MAE_EXIT_RATIO`.** Both affect the SL/exit
  geometry. They're tuneable in principle but the harness doesn't
  currently expose them. Phase 1 ignores them; if Phase 2 results are
  marginal these become Phase 2b candidates with a separate harness
  extension.

- **Cross-symbol contamination.** Changing USTEC.F params does not
  affect SP/EURUSD overrides at engine_init.hpp:605-607/661-663. The
  GER40 settings at 632-634 are independent (state A by definition,
  part-K audit). No cross-engine review required.

---

## 9. Effort estimate

| Phase | Wall-clock | Cumulative |
|---|---|---|
| 0 (harness extension) | 15 min | 0:15 |
| 1 (univariate, 21 runs) | 30 min | 0:45 |
| 2 (2D refinement, ~12 cells) | 20 min | 1:05 |
| 3 (WF, 3 folds × top-3 candidates) | 45 min | 1:50 |
| 4 (S63 grid, ~40 cells, if applicable) | 30 min | 2:20 |
| 5 (results write-up + engine_init edit + commit) | 20 min | 2:40 |

**~2.5-3 hours one operator session.** Most of it is wall-clock sweep
time; agent attention is light during the sweeps (kick off → wait →
aggregate).

---

## 10. Pre-flight checklist (run at start of next session)

Before any code touches:

1. Confirm `backtest/VWAPReversionBacktest.cpp` builds clean on Mac
   (`cmake --build build --target VWAPReversionBacktest --config Release
   -j`).
2. Confirm the NSXUSD tape file exists at the expected path (or the
   fresh Dukascopy pull is queued / completed).
3. Confirm `g_vwap_rev_nq.enabled = false` in engine_init.hpp — this
   is the precondition for the whole exercise (re-enable is the
   eventual goal, not the start state).
4. Re-read engine_init.hpp:597-672 to confirm no later commits revised
   the part-K/L revert evidence; the memo above assumes those comments
   are still load-bearing.

---

## Appendix: harness CLI extension diff (illustrative)

Not yet applied. Reference for Phase 0 implementation:

```cpp
// Around L386 (declarations):
bool have_ext_ovr = false, have_max_ext_ovr = false;
bool have_max_hold_ovr = false, have_cooldown_ovr = false;
double ext_ovr = 0.0, max_ext_ovr = 0.0;
int    max_hold_ovr = 0, cooldown_ovr = 0;

// Around L401 (CLI parse loop):
else if (!std::strcmp(argv[i], "--ext")      && i + 1 < argc) { ext_ovr      = std::atof(argv[++i]); have_ext_ovr      = true; }
else if (!std::strcmp(argv[i], "--max-ext")  && i + 1 < argc) { max_ext_ovr  = std::atof(argv[++i]); have_max_ext_ovr  = true; }
else if (!std::strcmp(argv[i], "--max-hold") && i + 1 < argc) { max_hold_ovr = std::atoi(argv[++i]); have_max_hold_ovr = true; }
else if (!std::strcmp(argv[i], "--cooldown") && i + 1 < argc) { cooldown_ovr = std::atoi(argv[++i]); have_cooldown_ovr = true; }

// Around L427 (apply overrides):
if (have_ext_ovr)      p.EXTENSION_THRESH_PCT = ext_ovr;
if (have_max_ext_ovr)  p.MAX_EXTENSION_PCT    = max_ext_ovr;
if (have_max_hold_ovr) p.MAX_HOLD_SEC         = max_hold_ovr;
if (have_cooldown_ovr) p.COOLDOWN_SEC         = cooldown_ovr;
```

Banner update at L447-453 should append corresponding override-tag
suffixes mirroring the existing S63 trio.

---

*End of plan. No code committed; this memo is the carry-over artifact
for the next VWR-USTEC.F session.*
