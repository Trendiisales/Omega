# BIGCAP-45 "any other engine" full scan — findings (S-2026-07-17k)

**Ask (operator):** full check of every remaining engine candidate runnable on the 45-name
BIGCAP daily-OHLC corpus, full table + explanation, same rigor as the XS-rotation readout.
**Harness:** `backtest/bigcap45_engine_scan_bt.py` (+ `--peryear`, `--passlist`).
**Data:** `backtest/data/bigcap_daily_ohlc/` 45/45 + SPY + QQQ, 10y (2016-07..2026-07),
scanned clean 17j. Point-in-time inclusion (late IPOs enter when history suffices).
**Costs:** per-name 16bp RT (2× = 32bp); portfolio 8bp/side on turnover (2× = 16bp).
**SURVIVORSHIP CAVEAT (rides everything):** universe chosen 2026 after these names won.
Absolute returns are inflated by construction. Decision metric = alpha vs the SAME-universe
gated-EW45 control (portfolio cells) / per-name all-6 robustness (trade cells), NEVER raw net.

## Candidate space and why each was in/out

Excluded up front: XS momentum rotation (DEAD 17j — 2024 concentration, no ranking alpha
ex-2024), PEAD (no earnings dataset), overnight-gap / VWAP / shorts (rejected prior session),
pairs-within-45 (needs a short leg — rejected class). Tested: A dip-extension, B turtle-
extension, C 52wk-high proximity, D dual momentum, E low-vol tilt, F gap-continuation o→c.

## Verdict table (1× cost; 2× in scan output — every verdict 2×-robust unless noted)

| Cand | Cell | n | net | PF/Sharpe | 2022 | ex-2024 | WF both | vs control | VERDICT |
|---|---|---|---|---|---|---|---|---|---|
| A | dip NEW-34 pooled | 1569 | +818% | PF 1.30 | −131 | +482 | YES | — | selective only |
| A | **dip ext-11 PASS set** | 700 | +560% | PF 1.67 (2×: 1.51) | −56 | +434 | YES | — | **EXTEND candidate** |
| B | turtle NEW-34 pooled | 1506 | +7076% | PF 2.20 | −572 | +4676 | YES | — | selective only |
| B | **turtle ext-14 PASS set** | 733 | +3774% | PF 2.59 (2×: 2.50) | −162 | +2304 | YES | — | **EXTEND candidate** |
| C | **hi52 within-5%** | port | +859% | sh 1.27 / shEx24 1.10 | −11.1% | +493% | YES | sh +0.09, shEx24 +0.12, mdd −2.3pt, 2022 +7.6pt | **VIABLE-SHADOW candidate** |
| D | dualmom K=5 | port | +5641% | sh 1.29 / shEx24 1.08 | −22.4% | +1482% | YES | 2025 = **−7.5% vs control +22.9%**, mdd 43.8% | NOT PROVEN (XS-class concentration) |
| D | dualmom K=10 | port | +1789% | sh 1.17 / shEx24 0.92 | −22.7% | +561% | YES | shEx24 < control | DEAD |
| E | lowvol K=10/15 | port | +239/210% | sh 0.99/0.92 | −17% | — | YES | sh ≪ control 1.18 | DEAD |
| F | gap-cont o→c 1/2/3% | 9872/3956/2022 | −1404/−648/−600% | PF 0.89/0.91/0.86 | neg | neg | NO | — | DEAD all cells |

Control (the null every 45-name candidate must beat): gated-EW45 sh 1.18, shEx24 0.98,
mdd 31.7%, 2022 −18.7%, ex24tot +345%.

## A+B — extensions of the two LIVE archetypes (the actionable result)

Standing order "extend winning engines": StockDip (RSI2<10 above SMA200 → SMA5-bounce/10d)
and StockTurtle (Donchian 20/10 close channel) run live on 11+11 names; the other ~26 names
were NEVER swept. Live-11 sanity rows reproduce in this harness (dip PF 1.79, turtle PF 2.06 —
shape matches certified). House all-6 gate applied per name (n≥30, net>0, PF≥1.3, H1>0,
H2>0, ex-best>0), 16bp RT:

- **TURTLE extension PASS-14:** MU MRVL PLTR TSLA META NFLX CRWD PANW DELL AMZN GOOGL ASTS
  RKLB WDC. Pooled PF 2.59, 2× PF 2.50, both halves+, ex-2024 +2304. 2022 pooled −162
  (≈ −12/name — the live-11 roster was flatter in the bear; SPY-gate/regime question is per
  wire decision). Note MU/DELL are live DIP names — no conflict, different archetype.
- **DIP extension PASS-11:** MRVL PLTR META NFLX SHOP MSTR NOW AMZN GOOGL WDC DD.
  Pooled PF 1.67, 2× PF 1.51, both halves+, ex-2024 +434, 2022 −56.
- Late IPOs (ARM COIN IONQ RGTI QBTS NBIS CRWV ALAB CRDO + SNOW dip) fail n≥30 — re-sweep
  when history accrues. Notable near-misses: MSTR turtle (PF 2.20, H1 −79 fails), SMCI turtle
  (ex-best −9.6 = one-trade dependent), ADBE dip (H2 −9.2).
- Overlap control: PASS names exclude each archetype's own live roster by construction;
  book overlap with the OTHER live archetype exists only via MU/DELL (turtle-ext vs live dip)
  and DD/WDC-class dual-passes — separate engines, separate books, additive (companion rule).

## C — 52wk-high proximity (the one NEW archetype that beats the control)

Hold (equal-weight) every name within 5% of its 52wk high, weekly rebal, SPY-200DMA gate.
No top-K concentration bet — membership is threshold-based, so it dodges the XS-rotation
failure mode. Beats gated-EW45 on Sharpe (1.27 v 1.18), ex-2024 Sharpe (1.10 v 0.98), mdd
(29.4 v 31.7), 2022 (−11.1 v −18.7), 2×-robust (sh unchanged), both halves+. Per-year: wins
2019/20/21/22/24/26, loses 2017/18 (small) and 2023 (+16.8 v +29.1 — recovery year, names
still far off highs are excluded by design). Edge is modest and the survivorship caveat is
irreducible (near-high names in a universe of eventual winners), but unlike XS rotation the
edge does NOT vanish ex-2024 and does NOT lose 2025/2026.

## D/E/F — kills

- **Dual momentum K=5** repeats the XS signature: monster raw net (+5641%) with mdd 44%,
  2022 worse than control, and **2025 −7.5% vs control +22.9%** — concentration beta, not
  ranking alpha. K=10 drops below control ex-2024. NOT PROVEN, no wire.
- **Low-vol tilt** Sharpe 0.99 < control 1.18 on this universe (high-beta winners universe —
  low-vol selection just sheds the return engine). DEAD.
- **Gap-continuation o→c** negative at every threshold, both halves, before stress. DEAD.
  (Also would have needed intraday open fills the stock book can't execute.)

## Standing-instruction rider — stock5m ignition mimic (separate study, ran on pull-complete)

45/45 5m corpus (60d-depth, 2024-07..2026-07). PARENT ride2rev passes the house gate in all
three modes incl REAL×2. MIMIC4 passes on DAILY (control) but **fails house gate on both 5M
modes at REAL×2** (PF 1.06/1.16 < 1.3); pend-window sweep doesn't rescue it. Verdict: daily
mimic remains the certified shape; 5m mimic NOT PROVEN at stress cost. Full table:
`stock5m_ignition_mimic_bt.py` output (session task b132owtxx).

## Recommendation (operator decides wire)

1. **Turtle ext-14 + dip ext-11** — same engines, proven archetypes, house-gated per name,
   2×-robust: natural StockDipTurtleEngine roster additions (SHADOW, $10k notional, same
   retire thresholds recomputed per worst-DD).
2. **hi52-5%** — genuinely beats the control risk-adjusted incl ex-2024; needs a new
   (simple) portfolio engine; modest edge; SHADOW only if operator wants a third stock book.
3. D/E/F — no wire, tombstone-class findings.
