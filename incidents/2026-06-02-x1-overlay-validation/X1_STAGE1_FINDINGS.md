# X1 Overlay — Stage-1 Refinement Findings
**Date:** 2026-06-02 · **Symbol:** XAUUSD · **Data:** Dukascopy M1, 22,223 bars, 2026-05-11 → 2026-06-02
**Trades:** 100 of 109 XAUUSD fills in window (32 winners / 68 losers)
**Tooling:** `x1_stage1.py` (imports the fidelity-checked `x1_validate.py`; baseline reproduced exactly before this run)
**Status:** still SUGGESTIVE — but the headline is now sharper, and one confound is exposed.

---

## Cut 1 — Per-engine / per-family confirm-gap

Confirming momentum tag in the prior 10 bars. `gap = win_conf − los_conf` (the real signal).

| family   | n  | win | los | win_conf | los_conf | gap     |
|----------|----|-----|-----|----------|----------|---------|
| trend    | 46 | 16  | 30  | 68.8%    | 56.7%    | **+12.1** |
| meanrev  | 24 | 1   | 23  | 0.0%     | 34.8%    | −34.8 (n_win=1, ignore) |
| scalp    | 24 | 10  | 14  | 70.0%    | 64.3%    | +5.7    |
| straddle | 6  | 5   | 1   | 100%     | 100%     | 0.0     |
| **ALL**  |100 | 32  | 68  | 71.9%    | 51.5%    | **+20.4** |

Per-engine (only DonchianBreakout & GoldScalpPyramid reach n≥3 in both classes):
- **DonchianBreakout** (trend): win 87.5% vs los 66.7% → **gap +20.8** (n 8/6)
- GoldScalpPyramid (scalp): win 75.0% vs los 63.6% → gap +11.4 (n 4/11)

### The important finding: the pooled +20.4 is partly a COMPOSITION artifact
The pooled gap (+20.4) is **bigger than any single family's within-family gap**. That can only
happen via composition: winners are disproportionately drawn from high-confirm families
(trend/scalp/straddle, 69–100% confirm) and losers disproportionately from the low-confirm
meanrev family (34.8%). So a chunk of the headline separation just encodes *which engine fired*,
not *which trade within an engine is better*.

- The honest **per-trade** filter edge is **~+12 pts (trend)**, not +20.
- The filter direction matches the hypothesis: it carries information for **trend** engines,
  is weak for scalp, and is meaningless/negative for mean-reversion (expected — those enter
  *against* momentum by design).
- **Build implication:** a *global* momentum-confirm gate would double-count the composition
  effect and is the wrong shape. The right shape is a **per-family gate, trend engines only**
  (DonchianBreakout the standout candidate).

---

## Cut 2 — Horizons matched to hold time

**Data-quality guard fired:** 9 trades dropped with `hold_sec ≈ 6.9e7` (~800 days) — all
`XauTrendFollow2h/4h/D1` cells. These are unclosed-exit artifacts in `omega_trade_closes.csv`,
not real holds. *(Worth a separate look at why those rows close with an ~800-day hold.)*

Median real hold per family → matched M1 horizon:

| family   | n  | median hold | matched horizon |
|----------|----|-------------|-----------------|
| trend    | 46 | 8.9 min     | 9 bars          |
| meanrev  | 24 | 10.0 min    | 10 bars         |
| scalp    | 24 | 0.8 min     | 1 bar           |
| straddle | 4  | 65.7 min    | 66 bars         |

### Finding: the "M1 understates edge" caveat is mostly VOID
Findings §4 worried the 5–20-bar forward test was too short for hours-long holds. With the 9
artifact holds removed, **real median holds are 9–10 minutes** — already inside the original
5/10/20-bar window. The hours-long holds were exactly the bogus rows.

Re-running the tag forward test at the matched horizons (1/9/10/66 bars) changes nothing
structural:
- **momentum tags** still ~47% hit at every horizon, mean wrong-sign — no standalone edge
  appears at longer horizons. Extending the horizon does **not** rescue the momentum tag.
- **retracement tags** still lean mean-revert (retr_up 56% / retr_down 57% at 9–10 bars),
  still ~1–2 bps — sub-spread, same as before.

So the tag's value remains as a **filter on trend entries**, not as a horizon-sensitive signal.

---

## Where this leaves Stage 1
- The 72%-vs-52% result survives but is **half level-of-engine, half within-trade**. The real,
  buildable part is the **+12-pt within-trend gap**, concentrated in DonchianBreakout.
- Still ~2 SE (trend: 16 winners). n is the binding constraint, not the analysis.
- Horizon-mismatch caveat resolved (real holds are short). One new data-quality item surfaced
  (the ~800-day `XauTrendFollow` closes).

### Recommended next move
Skip further M1 re-slicing (n won't grow offline). Go to **Stage 2 shadow logging**, but scope
the eventual gate hypothesis to **trend engines only** from the start — log `wt1_at_entry`,
`regime_up_at_entry`, `momentum_confirm` into the trade log, and accumulate fresh fills until
the **within-trend** (not pooled) gap clears noise on out-of-sample data. Gate decision (Stage 4)
stays per-family, DonchianBreakout-first.
