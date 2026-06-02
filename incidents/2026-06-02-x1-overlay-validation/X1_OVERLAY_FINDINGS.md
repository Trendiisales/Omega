# X1 Overlay — Validation Findings & Roadmap
**Date:** 2026-06-02 · **Symbol:** XAUUSD · **Data:** Dukascopy M1, 2026-05-11 → 2026-06-02 06:59 UTC (22,223 bars)
**Trades:** omega_trade_closes.csv, 100 of 109 XAUUSD fills in window (32 winners / 68 losers)
**Status:** SUGGESTIVE (≈2 SE) — promising, not validated. Do not deploy live.

---

## 1. Indicator definitions (as built)
WaveTrend (LazyBear) on hlc3, defaults n1=10, n2=21, signal SMA=4, OB ±53/±60.
Trend regime: EMA(close,21) vs EMA(close,55).
All tags computed on CLOSED bars only (shift(1)) → non-repainting by construction.
- momentum_up   = bullish wt1×wt2 cross AND regime up
- momentum_down = bearish wt1×wt2 cross AND regime down
- retr_down     = regime up AND first touch wt1 ≥ +53  ("price may dip")
- retr_up       = regime down AND first touch wt1 ≤ −53 ("bounce")

Tag counts in window: momentum_up 690, momentum_down 787, retr_down 232, retr_up 245.

## 2. Forward-return validation (return over N bars after each tag)
Units: bps. hit% = P(move in the tag's intended direction).

| tag           | 5-bar hit | 10-bar hit | 20-bar hit | 5/10/20 median bps |
|---------------|-----------|------------|------------|---------------------|
| momentum_up   | 47.0%     | 47.8%      | 46.4%      | −0.36 / −0.56 / −0.73 |
| momentum_down | 48.2%     | 48.2%      | 49.3%      | +0.41 / +0.40 / +0.12 |
| retr_down     | 51.7%     | 56.9%      | 53.0%      | −0.27 / −1.51 / −1.84 |
| retr_up       | 57.1%     | 56.3%      | 53.5%      | +0.94 / +1.41 / +2.17 |

Read: momentum tags are noise-to-wrong as standalone predictors. Retracement
tags lean correctly (mean-reversion) but the magnitude (~1–2 bps) is below the
~$0.30 gold spread → not tradeable on its own at M1.

## 3. Entry-filter result (the value)
Confirming momentum tag present in the 10 bars BEFORE entry:
- Winners: 71.9%
- Losers:  51.5%
~20-point separation. The momentum tag carries information about which entries
work, even though it is not a standalone signal. This is the integration thesis:
overlay as a GATE on existing engine signals, not as a signal itself.

## 4. Why this is not yet proof
- Mechanical confound: trend engines enter with momentum by design; the
  winners>losers GAP is the signal, not the absolute 71.9%.
- n: 32 winners, n≈232 retracement events → gaps ≈ 2 SE. Underpowered.
- Horizon mismatch: 5–20 min forward vs hours-long holds (Tsmom_H1/H2/H4).
  The M1 forward test likely understates edge realised over the real hold.

## 5. REFINEMENT BACKLOG (Stage 1, offline)
1. Horizons matched to hold time: re-run forward returns at H1/H2/H4 bars and at
   each engine's median hold_sec.
2. Per-engine breakdown of the confirm-filter (hypothesis: helps Tsmom/Donchian/
   EmaPullback; neutral-or-hurts MidScalper/MeanReversion/scalp engines).
3. Param sweep WaveTrend (incl. the chart's 35/10/25/25) for filter separation,
   ranked by winner−loser gap, with a stability guard (avoid curve-fit).
4. Redefine the momentum tag — current wt-cross is weak standalone; test wt1
   zero-cross, wt1 slope, and WaveTrend divergence as alternative gates.
5. More data: extend window as live fills accumulate; add out-of-sample months
   where engines were live.
6. Multi-symbol: US500.F/USTEC.F/GER40 (Dukascopy USA500.IDXUSD / USATECH.IDXUSD
   / DEU.IDXEUR, own --scale), GBPUSD/EURUSD (--scale 100000).
7. Build the "X1 Flash" reversal oscillator (2nd component, not yet implemented):
   normalized momentum wave + extreme triangles + heat-zone band.

## 6. INTEGRATION PLAN (staged, gated, change-control safe)
- Stage 0 (DONE): offline research tooling. No core code touched.
- Stage 1: complete Refinement Backlog items 1–4 → confirm the filter edge is
  real and per-engine. GATE: winner−loser confirm gap holds with adequate n.
- Stage 2 (shadow-only): add a READ-ONLY module that computes WaveTrend +
  momentum/regime state on the live feed and writes it as new columns into the
  trade log (e.g. wt1_at_entry, regime_up_at_entry, momentum_confirm). NO order
  gating. Purpose: accumulate forward out-of-sample evidence on real fills.
  GATE: fresh live fills reproduce the winners>losers separation.
- Stage 3 (GUI): render oscillator sub-panel + tags in the Omega GUI
  (7779 HTTP / 7780 WS). Read-only monitoring.
- Stage 4 (filter gate): only after Stage 2 confirms on fresh data — gate
  specific engines' entries on momentum_confirm, behind a config flag,
  shadow-first, full pre-delivery checklist, reversible.

## 7. Files
- x1_validate.py        — validation engine (CLI flags for all params)
- pull_dukascopy.py     — Dukascopy tick → bars downloader
- XAUUSD_2026-05_m1.csv — regenerated bars (data; not committed to repo)
- xau_may_real.png      — annotated chart
