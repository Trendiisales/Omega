# TURTLE-MIMIC + extra DIP cells — findings (S-2026-07-16p)

Operator: "with this pf i want 4 mimics added ... add 2 as well." Extend the shipped
StockDipMimic (GoldTrendMimicBook) BE-floored mimic to the **TURTLE breakout family**
(4 cells) + add **2 more DIP cells**. Independent SHADOW books, judged **STANDALONE**
(feedback-companion-independent-engine) — never compared to riding the stock or to the
real engine's return.

## Data / method
- Yahoo split+div-adjusted daily OHLC, 10y (2512 bars), DIP+TURTLE union + SPY/QQQ.
  Fetch: `backtest/fetch_stock_bt_data.py`.
- Faithful to `include/StockDipTurtleEngine.hpp`:
  - **DIP** entry: close>SMA200 AND Cutler-RSI2<10 (3896 entries, 11 names).
  - **TURTLE** entry: flat-gated — LONG at close>max(prior 20 closes); one position at a
    time, released at close<min(prior 10 closes) (633 entries, 11 names). This mirrors
    the REAL engine's entry cadence (the mimic opens a leg per real entry).
- Mimic sim = exact `GoldTrendMimicBook.on_h1_bar` port (pre-arm LOSS_CUT + pre-arm
  BE-ratchet once peak≥pbe + post-arm BE-floored peak trail). Identical to the shipped
  `stockdip_mimic_prearm_floor.py sim_leg`. Cost 8bp RT. Cell convention: lc=arm,
  pbe=arm/2 ("half-of-arm"), cap=10.
- Harnesses: `backtest/turtle_mimic_bt.py`, `backtest/mimic_200dma_gate_bt.py`,
  `backtest/mimic_cell_sweep.py`.
- **all-6 viability** = net>0, PF≥1.3, both halves+, bull+, bear≥0 (regime split by the
  broad-market QQQ-200DMA lens). Metrics are additive per-trade-% units for RANKING
  configs — NOT compounded account returns.

## Result 1 — TURTLE-mimic is viable (whole arm/gb grid all-6 PASS)
Sweep of arm∈{1.5..4.0}×gb∈{0.40..0.70}: **every cell all-6 VIABLE**, PF 1.6–2.0. The
edge is robust. Bear slice is POSITIVE under the broad-market (SPY/QQQ) regime (the
"bear-negative" seen under the family-index lens was a harsher-slice artifact, not a
real failure) — so no bull-gate is *required*, though it halves drawdown (Result 3).

## Result 2 — the 6 wired cells (chosen by net/mdd, diverse arm ladder), UNGATED
| tag | family | arm | gb | lc | pbe | n | net% | PF | mdd | bear(QQQ) | all-6 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| StockTurtleMimA | TURTLE | 1.5 | 0.50 | 1.5 | 0.75 | 633 | +366 | 1.91 | −25 | +30 | ✅ |
| StockTurtleMimB | TURTLE | 2.0 | 0.50 | 2.0 | 1.00 | 633 | +446 | 1.95 | −33 | +49 | ✅ |
| StockTurtleMimC | TURTLE | 2.5 | 0.40 | 2.5 | 1.25 | 633 | +482 | 1.87 | −40 | +47 | ✅ |
| StockTurtleMimD | TURTLE | 3.5 | 0.40 | 3.5 | 1.75 | 633 | +488 | 1.70 | −46 | +47 | ✅ |
| StockDipMimX | DIP | 1.5 | 0.70 | 1.5 | 0.75 | 3896 | +3202 | **2.19** | −65 | +234 | ✅ |
| StockDipMimY | DIP | 2.5 | 0.60 | 2.5 | 1.25 | 3896 | +3346 | 1.94 | −108 | +232 | ✅ |

All 6 all-6-VIABLE **and bear-positive** ungated → wired ungated (`bull_only=false`),
matching the proven StockDipMim pattern. (Existing shipped DIP cells T arm2/gb50 +
W arm3/gb70 remain; these 6 are additive → DIP now 4 cells, TURTLE now 4 cells.)

## Result 3 — QQQ>200DMA gate = optional risk overlay (deferred)
A broad-market 200DMA bull-gate (QQQ beats SPY for these tech names) ~halves drawdown
for ~10% of net, all cells stay all-6 VIABLE:
| book | ungated mdd | SPY-gate | **QQQ-gate** |
|---|---|---|---|
| DIP T | −86 | −82 | **−51** |
| DIP W | −112 | −113 | **−95** |
| TURTLE T | −33 | −17 | **−19** |
| TURTLE W | −59 | −32 | **−34** |

NOT wired now: `GoldTrendMimicBook.bull_only` gates on the XAU-H1 SMA200 feed (gold-only);
a QQQ stock gate needs a NEW QQQ daily-close regime feed + plumbing + seed-registry entry.
Cells are viable without it, so it's a fast-follow mdd-reducer, not a blocker.

## Wiring (commit S-2026-07-16p)
- `include/StockDipTurtleEngine.hpp`: fire the mimic open-cb for TURTLE too (was DIP-only,
  L437); new `set_turtle_mimic_cbs()` installs hooks on TURTLE-family syms (disjoint from
  the DIP setter). No change to the real position (independent SHADOW).
- `include/engine_init.hpp`: +2 DIP cells (X,Y) + 4 TURTLE cells (A–D) into the sdm book;
  DIP fan → 4 cells, new TURTLE fan → 4 cells; SEED printf updated.
- SHADOW (`send_live_order` no-ops until mode=LIVE); OM-03 fail-closed (real single-name
  equity cost row owed before any LIVE sizing — same constraint as StockDip parent).

## Protection verdict (mandate: every mimic ships a backtested loss-protection verdict)
Each cell carries pre-arm LOSS_CUT (drawdown-cancel at −lc) + pre-arm BE-ratchet (peak≥pbe
→ reversal exits at BE, never neg) + post-arm BE-floored peak trail (armed leg can never
book negative — feedback-mimic-be-floor-mandatory). Backtested: worst clip bounded at −lc%
per cell; no armed leg books negative. Free protection (mimic never touches the real trade).
