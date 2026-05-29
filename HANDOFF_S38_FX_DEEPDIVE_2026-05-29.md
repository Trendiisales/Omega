# FX Deep-Dive 2026-05-29 — Why Most Pairs Can't Be Made To Work

Diagnostic results, root cause, and what's actionable.

## Method
- Built `backtest/fx_diagnostic.cpp` — measures intrinsic edge per pair:
  return distribution stats (mean, std, skew, kurt), lag-1 autocorrelation,
  buy-and-hold Sharpe, and **oracle Sharpe** (perfect-foresight directional).
- Built `backtest/fx_diagnostic_d1.cpp` — D1 Donchian at N={20, 55, 200}
  + long-only-with-trail Donchian. Cost sensitivity: c=0, 0.25x, 0.5x, 1.0x.
- Extended `backtest/fx_edge_sweep.cpp` with `OVERLAY_SIGN` env var to
  cleanly invert the overlay direction. Tested per-pair overlays:
  AUD-Gold (±), CAD-Brent (±), EUR-USDJPY-inverse (DXY proxy), GBP-USDJPY-inverse.

## H4 Diagnostic Table

| Pair | bars | ret_μ (pips) | ret_σ | autocorr1 | BH | Oracle | Donch c=0 | Donch c=1× |
|---|---|---|---|---|---|---|---|---|
| EURUSD | 1790 | 0.73 | 21.4 | -0.017 | **1.45** | 40.9 | -0.53 | -7.93 |
| GBPUSD | 2044 | 0.46 | 23.9 | +0.012 | 0.87 | 44.4 | **+1.10** | -5.57 |
| USDJPY | 2044 | 0.09 | 35.0 | +0.028 | 0.12 | 43.7 | -1.48 | -4.76 |
| AUDUSD | 2153 | 0.46 | 16.0 | -0.017 | **1.32** | 44.3 | -0.96 | -12.75 |
| NZDUSD | 2154 | 0.13 | 14.0 | +0.019 | 0.43 | 47.5 | **+1.18** | -9.97 |
| USDCAD | 2153 | -0.35 | 19.8 | -0.021 | -0.83 | 41.1 | -0.23 | -10.08 |
| EURGBP | 2044 | 0.22 | 10.3 | -0.012 | 0.94 | 40.5 | -0.35 | -16.06 |

## Root Cause — Four Independent Findings

### 1. Autocorrelation lag-1 ≈ 0 across all pairs
Range -0.021 to +0.028. **FX H4 bar-to-bar moves are near pure random
walk.** No persistence -> trend-following has no fuel. No reversal -> mean-
reversion has no fuel. Same pattern at H1 (range -0.05 to +0.02).

### 2. Oracle Sharpe 40-47 vs Donchian Sharpe -8 to +1
Massive information exists if you could call sign of next bar.
But ac1≈0 means **you can't from price alone**. Capture requires
external signal (cross-asset, news, microstructure).

### 3. Cost destroys the only two raw edges
GBPUSD H4 Donch c=0 = +1.10. NZDUSD H4 Donch c=0 = +1.18. Real raw edges.
At cost 0.25x ($0.09/rt): GBPUSD -0.57, NZDUSD -1.61. **Cost kills 1.5-2.5
Sharpe per quarter-cost step.** Edge exists but cost-to-noise ratio is
catastrophic at 0.01 lot. At 0.10 lot the spread/slip components scale
linearly but the $6 fixed commission shrinks % impact -- which is what
makes USDJPY+SPX viable.

### 4. Buy-and-hold Sharpe 1.3-1.5 on EUR/AUD = passive drift exists
But it's CAPTURED BY HOLDING, not by active trading. Donch -0.5 to -1
on the same pairs = the Donchian rule mis-times the drift. Long-only-
with-trail D55 fares no better (Sharpe -2.7 to -9.1, see d1 diag).

## Per-Pair Overlay Tests

Cross-asset overlays tested using `fx_edge_sweep.cpp` SPX_Conditional /
SPX_VolGated families repurposed via `--spx-csv <other_asset_h4>` and
`OVERLAY_SIGN={+1,-1}`.

| Pair | Overlay | Sign | Best Full Sharpe | OOS Sharpe | Verdict |
|---|---|---|---|---|---|
| USDJPY | SPX | +1 | 2.58 (H4) | **2.60** | **WORKS** -- 4 cells WF-valid, ship |
| GBPUSD | USDJPY (DXY proxy) | -1 | 1.04 (D1 n=23) | -1.04 (OOS n=14) | IS-fit only, OOS collapse |
| EURUSD | USDJPY (DXY proxy) | -1 | -0.77 (best) | -- | Fails IS |
| AUDUSD | Gold | +1 | -2.21 (best) | -- | Fails IS |
| AUDUSD | Gold | -1 | -4.71 (best) | -- | Fails IS both ways |
| USDCAD | Brent | -1 | -3.61 (best) | -- | Fails IS |
| USDCAD | Brent | +1 | (negative) | -- | Fails IS |
| NZDUSD | Gold | +1 | (negative) | -- | Fails IS |

Only USDJPY-SPX survives. GBPUSD-USDJPY looked promising but doesn't
hold OOS -- the GBP-USD regime that drove IS edge (USD-strength Q3'25)
flipped Q4'25-Q1'26 (USD-weakness).

## Why USDJPY Works When Others Don't

JPY is the unique high-quality risk barometer because:
1. **BoJ rates pinned for 30+ years** -- USDJPY moves are pure
   USD-dollar-strength + risk-on/off, not BoJ policy.
2. **JPY is the canonical funding currency** -- when SPX rises, leverage
   builds, JPY shorts grow -> USDJPY up. Inverse on risk-off.
3. **Liquidity at H4+** is deep enough that the regime signal isn't
   noise-dominated.

EUR/GBP/AUD/NZD/CAD all have **idiosyncratic central bank policy** as a
major price driver in 2024-26 (ECB cuts, BoE holds, RBA pauses, BoC cuts
ahead-of-Fed, RBNZ aggressive cuts). That overlays on top of risk-on/off
and breaks the simple cross-asset rule.

## Why The 2024-26 Window Is Particularly Hostile

- Fed/ECB/BoE/BoJ/RBA/RBNZ/BoC all on different rate paths -> no shared
  macro driver -> cross-pair correlations decoupled.
- USDCAD specifically: oil drifted (Brent flat 70-85) while CAD weakened
  on BoC over-cutting -> oil-overlay broke.
- 2022 dollar surge + 2023 reversal already played out -> 2024-26 is the
  noisy aftermath. Carry differentials shrunk as rates normalized.
- Verified by:
  - EUR/AUD passive Sharpe 1.3-1.5 (slow drift, mostly bullish bias)
  - USDCAD passive Sharpe -0.83 (slow CAD weakness)
  - Everything else passive Sharpe ~0 (range-bound)

## What's Actionable

### Ship USDJPY+SPX (4 walk-forward survivors, already validated)
- `g_usdjpy_spx_trend` new engine. SPX EMA20 > EMA100 = long, < = short.
  ATR > 1.2x median ATR_50 gate. SL=2xATR, TP=5xATR.
- ~70 trades/yr, $570/yr at 0.10 lot on full data. OOS Sharpe 2.60.

### Pursue these for a future sweep (not this session)

1. **FRED rate-differential data + carry overlay.** 2-year Treasury vs
   foreign 2-year. Classical FX carry edge. Effort: ~2h data fetch +
   harness extension. Expected: re-test EUR/AUD/NZD/GBP at D1+ with
   carry as primary signal.

2. **News-event harness.** NFP day, CPI day, central-bank meeting day.
   Hold positions only across event windows. Effort: ~3h. Expected:
   high-confidence signals on event days only (~50 trades/yr/pair).

3. **FX L2 logging on VPS.** Currently L2 only logged for XAU/USTEC/
   US500/NAS100. Adding EUR/GBP/USDJPY L2 captures takes ~30min code
   in `on_tick.hpp` (mirror existing pattern). Need 3 months data
   before meaningful microstructure sweep.

4. **Pair-trading EURUSD vs GBPUSD z-divergence.** Hedged, cost doubled
   but currency exposure cancels. Sharpe target 0.7-1.0 historically.
   Effort: ~2h custom harness.

### What WILL NOT work (don't waste cycles)

- Adding more bracket families (Bollinger, RSI, Mom, ZScore, MACross
  variants) to the existing FX sweep. All tested. All fail.
- Lower timeframes (M5, M15) without microstructure data. Cost ratio
  gets WORSE not better -- spreads dominate at scalp TFs.
- Reducing cost assumptions. OmegaCostGuard already reflects production
  BlackBull ECN values. Going lower = unrealistic.
- Forcing EUR/CAD/NZD/EURGBP positive via param-fit. They have no edge
  in this window's data with available signals; IS-fit will collapse OOS.

## Files Touched This Diagnostic

- `backtest/fx_diagnostic.cpp` (NEW) -- intrinsic edge measurement
- `backtest/fx_diagnostic_d1.cpp` (NEW) -- D1 + long-only-with-trail
- `backtest/fx_edge_sweep.cpp` -- added `OVERLAY_SIGN` env var
- `HANDOFF_S38_FX_DEEPDIVE_2026-05-29.md` (this doc)
