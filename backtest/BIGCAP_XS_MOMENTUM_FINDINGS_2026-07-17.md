# BIGCAP cross-sectional momentum rotation — NOT PROVEN, no wire (2026-07-17)

**Question (operator-greenlit, S-2026-07-17i handoff item 3):** rank the 45 BIGCAP names against
each other, long top-K (3-8), inverse-vol, flat when SPY<200DMA — a distinct orthogonal engine
(nothing in the book ranks names against each other)?

**Answer: the raw numbers look spectacular but the RANKING ALPHA IS NOT PROVEN.** Outside 2024,
risk-adjusted return equals just holding the gated basket. No engine build / no wire. The handoff's
own caveat ("45 high-beta megacap = can degenerate into levered beta") is confirmed in the honest
decomposition.

## Harness

`backtest/bigcap_xs_momentum_bt.py` — Jegadeesh-Titman score `c[t]/c[t-lb]-1` (the
`cross_sectional_relval.py` / `CrossSectionalIndexEngine` archetype), long top-K, equal or
inverse-vol(20d) weights, SPY-200DMA gate (daily exit), close-to-close, 8bp/side on turnover
(2x = 16bp), point-in-time inclusion (late IPOs enter at lb bars: CRWV/ALAB/ARM/NBIS...).
Sweep lb{30,60,120,250} x K{3,5,8} x rebal{10,14,21} x weight{eq,iv}. Data:
`backtest/data/bigcap_daily_ohlc/` 45/45 + SPY/QQQ (WDC was MISSING — fetched this session;
fetch script roster fixed 39→45), all scanned clean (no zeros/h<l; big jumps = real earnings moves).

**Controls (the decision metric):** gated-EW45 (same universe, same gate, no ranking) +
EW45 buy&hold + QQQ buy&hold. The universe was chosen in 2026 AFTER these names won —
EW45 b&h = +1910% (38.7% CAGR) says absolute returns are hindsight-inflated by construction.
Only the ranked-book-vs-gated-EW comparison is meaningful.

## Headline sweep (8bp; 2x cost barely moves anything — turnover is low)

- Controls: gated-EW45 +781% sh1.18 mdd43% 2022 −26.4% | EW45 b&h +1910% sh1.21 mdd51% 2022 −45% | QQQ +457% sh0.92 mdd35% 2022 −33%.
- Best region: FAST lookback 20-50 (the index-XS "momentum wants slow 120-250" law does NOT
  transfer to single high-beta names). lb=30 K=5 rb=14 iv: +7186% sh1.41 mdd34% 2022 −20.5%
  both-halves+. Neighbors lb20-50 all sh 1.15-1.52 — no isolated spike, surface is broad.
- Every cell of the 72 both-halves-positive; all beat gated-EW on total return; iv-weight
  dominates eq on mdd.

## The honest decomposition (why NOT PROVEN)

Yearly, lb=30 K=5 rb=14 iv vs gated-EW45:

| year | strat | gated-EW |
|---|---|---|
| 2017 | +3.5% | +18.1% |
| 2018 | +19.0% | +12.5% |
| 2019 | +33.2% | +21.6% |
| 2020 | +120.2% | +53.8% |
| 2021 | +69.7% | +33.5% |
| 2022 | **−20.5%** | −25.8% |
| 2023 | +91.8% | +54.6% |
| 2024 | **+617.7%** | +94.2% |
| 2025 | +22.8% | +25.4% |
| 2026 | +2.2% | +8.7% |

- **2024 is the whole story** (+618% — MSTR/PLTR/NVDA mania, i.e. exactly the names the 2026
  roster was picked FOR). Ex-2024: strat +1074% vs control +419% on total, but **ex-2024 Sharpe
  1.02 vs 1.04 — the ranking adds NOTHING risk-adjusted outside that one year.** The extra
  return is pure concentration beta (K=5 of 45), paid for with proportional vol.
- **Loses to the plain gated basket in 2025 AND 2026** — the two most recent years.
- **2022: −20.5%.** Better than every control (gate + ranking soften the bear) but the gate does
  NOT cleanly flatten (SPY-200DMA whipsaw). A long-only rotation book bleeds double-digits in a
  2022 regime. Reported per standing rule; bull-gate framing applies (this IS the gated version).
- Survivorship: irreducible with this roster. The only test that could rescue the idea is a
  POINT-IN-TIME universe (e.g. top-45 US tech by mcap per year, delisted names included) — that
  dataset does not exist in-house.

## Verdict

- **No C++ engine, no wire, no shadow.** Standalone gates technically pass (net+, both halves,
  2x-cost) but the alpha claim fails the control: ex-2024 risk-adjusted parity with gated-EW =
  the ranking is not doing the work; the universe + the gate are.
- The existing per-name books (StockDip/Turtle/2pct ladder) already monetize this universe on
  each name's OWN signal without the concentration tail.
- Re-open ONLY with a point-in-time universe feed (kills the survivorship objection) — until
  then this is tombstone-class: "hindsight-universe rotation, alpha = 2024 concentration".
