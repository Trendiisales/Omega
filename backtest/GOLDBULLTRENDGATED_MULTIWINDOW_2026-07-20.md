# GoldBullTrendGated — FULL multi-window backtest grid (S-2026-07-20ai)

**Task (operator):** backtest the engine FULLY with a table across ALL /Users/jo/Tick XAU data.
**Verdict up front:** confirmed **BULL BETA, SHADOW** — the grid does NOT upgrade it to
all-weather. Strong in *trending* bull (6mo, 2024-26), gated to bounded losses in bears, but it
LOSES in one bull window (2026 Mar-Jun chop) and still bleeds modestly in some bears even gated.
It fails the all-weather bar ("net>0 across MULTIPLE bull AND flat/≥0 across bears"). Stays SHADOW.

## Method (honest)
- Harness `backtest/gold_ls_gated_bt.cpp` (extended this session with a `--barcsv` OHLC-bar reader
  in `gold_ls_harness.hpp::MinuteBarCsvReader` so m1/H1 bar files feed WITHOUT tick-collapsing the
  high/low). Honest worse-of fills, cost embedded, no look-ahead. LONG side reported (long-only engine).
- Cells locked to the shipped config: **DONCH** = bar60 / Donch20 / trail3×ATR / SMA400;
  **EMA** = bar30 / EMA20-50 / trail3×ATR / SMA800. Regime gate = shipped `gold_regime()` replica.
- Gate modes: **ungated** / **regime**-only / **regimeSMA** (= the SHIPPED behaviour).
- Every file passed `data_integrity_gate.py` (all CERTIFIED CLEAN; 2yr_XAUUSD_daily stays REJECTED, unused).
- PARITY CHECK: the two cert tapes reproduce the cert to the decimal — DONCH 6mo regimeSMA **+1126.3bp**,
  EMA 6mo regimeSMA **+784.3bp** (handoff figures). Harness + bar reader are sound.
- Windows: 6mo-bull (tick, cert), 2024-26 (m1), 2026mj (m1), 2023-24 (m1), 2013/2015/2022 (m1),
  2022tk (tick, cert, Jun-Sep slice). Duplicate-granularity files (H1 versions, ds10, spliced,
  m1_2022bear subset) folded — same windows, coarser; not re-listed to avoid muddying the table.

## DONCH cell (net_bp; cost 1×; h1/h2 = first/second half at 1×; 2x = full-cost net)
| window | reg | gate | n | net | PF | avgR | h1 | h2 | 2x_net | 2xPF |
|---|---|---|--:|--:|--:|--:|--:|--:|--:|--:|
| 6mo-bull | BULL | ungated | 61 | 2228 | 2.12 | 0.43 | 1430 | 799 | 1922 | 1.89 |
| 6mo-bull | BULL | regime | 57 | 1968 | 2.08 | 0.44 | 1403 | 565 | 1682 | 1.85 |
| 6mo-bull | BULL | **regimeSMA** | 49 | **1126** | 1.64 | 0.33 | 1033 | 94 | 881 | 1.46 |
| 2024-26 | BULL | ungated | 310 | 5811 | 1.72 | 0.32 | 1371 | 4440 | 4259 | 1.47 |
| 2024-26 | BULL | regime | 275 | 4888 | 1.68 | 0.31 | 1109 | 3780 | 3511 | 1.44 |
| 2024-26 | BULL | **regimeSMA** | 250 | **3740** | 1.54 | 0.26 | 607 | 3133 | 2488 | 1.33 |
| 2026mj | BULL | ungated | 35 | -4 | 1.00 | -0.06 | 27 | -30 | -179 | 0.89 |
| 2026mj | BULL | regime | 23 | -378 | 0.66 | -0.25 | 19 | -397 | -493 | 0.59 |
| 2026mj | BULL | **regimeSMA** | 12 | **-337** | 0.48 | -0.36 | 12 | -349 | -397 | 0.43 |
| 2023-24 | MIX | ungated | 170 | -390 | 0.90 | -0.07 | -541 | 151 | -1239 | 0.73 |
| 2023-24 | MIX | regime | 119 | 518 | 1.22 | 0.07 | 261 | 257 | -77 | 0.97 |
| 2023-24 | MIX | **regimeSMA** | 93 | **215** | 1.11 | 0.01 | 176 | 39 | -250 | 0.89 |
| 2013 | BEAR | ungated | 146 | 389 | 1.10 | 0.02 | -431 | 819 | -341 | 0.92 |
| 2013 | BEAR | regime | 89 | -68 | 0.97 | -0.01 | -212 | 144 | -513 | 0.80 |
| 2013 | BEAR | **regimeSMA** | 58 | **-138** | 0.91 | -0.02 | -319 | 182 | -427 | 0.75 |
| 2015 | BEAR | ungated | 145 | -498 | 0.86 | -0.06 | -325 | -173 | -1222 | 0.70 |
| 2015 | BEAR | regime | 86 | 26 | 1.01 | 0.06 | 112 | -86 | -404 | 0.84 |
| 2015 | BEAR | **regimeSMA** | 65 | **-384** | 0.78 | -0.06 | -163 | -221 | -709 | 0.64 |
| 2022 | BEAR | ungated | 153 | 28 | 1.01 | -0.01 | -299 | 327 | -737 | 0.82 |
| 2022 | BEAR | regime | 110 | 324 | 1.13 | 0.06 | -160 | 484 | -226 | 0.92 |
| 2022 | BEAR | **regimeSMA** | 80 | **386** | 1.19 | 0.09 | -29 | 415 | -15 | 0.99 |
| 2022tk | BEAR | ungated | 48 | -534 | 0.53 | -0.27 | -336 | -198 | -774 | 0.42 |
| 2022tk | BEAR | regime | 26 | -176 | 0.67 | -0.12 | 58 | -234 | -305 | 0.52 |
| 2022tk | BEAR | **regimeSMA** | 11 | **-13** | 0.92 | -0.10 | 92 | -105 | -68 | 0.63 |

## EMA cell
| window | reg | gate | n | net | PF | avgR | h1 | h2 | 2x_net | 2xPF |
|---|---|---|--:|--:|--:|--:|--:|--:|--:|--:|
| 6mo-bull | BULL | ungated | 52 | 1561 | 2.13 | 0.40 | 838 | 722 | 1300 | 1.84 |
| 6mo-bull | BULL | regime | 43 | 860 | 1.70 | 0.32 | 712 | 149 | 645 | 1.47 |
| 6mo-bull | BULL | **regimeSMA** | 33 | **784** | 1.98 | 0.36 | 651 | 134 | 619 | 1.68 |
| 2024-26 | BULL | ungated | 233 | 2738 | 1.56 | 0.21 | 533 | 2205 | 1572 | 1.28 |
| 2024-26 | BULL | regime | 171 | 1318 | 1.34 | 0.15 | 141 | 1177 | 463 | 1.10 |
| 2024-26 | BULL | **regimeSMA** | 146 | **1468** | 1.46 | 0.22 | 265 | 1204 | 738 | 1.20 |
| 2026mj | BULL | ungated | 40 | -772 | 0.49 | -0.39 | -192 | -580 | -971 | 0.42 |
| 2026mj | BULL | regime | 23 | -482 | 0.43 | -0.40 | -274 | -208 | -597 | 0.37 |
| 2026mj | BULL | **regimeSMA** | 10 | **-203** | 0.16 | -0.40 | -71 | -132 | -253 | 0.11 |
| 2023-24 | MIX | ungated | 120 | -431 | 0.80 | -0.14 | -508 | 77 | -1031 | 0.61 |
| 2023-24 | MIX | regime | 70 | -209 | 0.83 | -0.07 | -350 | 142 | -558 | 0.62 |
| 2023-24 | MIX | **regimeSMA** | 60 | **-189** | 0.83 | -0.05 | -369 | 179 | -489 | 0.63 |
| 2013 | BEAR | ungated | 106 | -1331 | 0.45 | -0.29 | -1143 | -188 | -1860 | 0.34 |
| 2013 | BEAR | regime | 54 | -694 | 0.38 | -0.32 | -525 | -170 | -964 | 0.26 |
| 2013 | BEAR | **regimeSMA** | 40 | **-490** | 0.45 | -0.26 | -358 | -133 | -690 | 0.32 |
| 2015 | BEAR | ungated | 107 | -847 | 0.61 | -0.25 | -562 | -285 | -1381 | 0.47 |
| 2015 | BEAR | regime | 42 | -80 | 0.90 | -0.11 | -216 | 136 | -290 | 0.70 |
| 2015 | BEAR | **regimeSMA** | 32 | **161** | 1.30 | 0.09 | 25 | 136 | 1 | 1.00 |
| 2022 | BEAR | ungated | 118 | 9 | 1.00 | 0.02 | -242 | 251 | -581 | 0.78 |
| 2022 | BEAR | regime | 67 | 113 | 1.09 | 0.11 | -173 | 286 | -222 | 0.86 |
| 2022 | BEAR | **regimeSMA** | 43 | **101** | 1.11 | 0.18 | -53 | 153 | -114 | 0.89 |
| 2022tk | BEAR | ungated | 37 | -362 | 0.51 | -0.32 | -173 | -189 | -546 | 0.38 |
| 2022tk | BEAR | regime | 20 | -180 | 0.57 | -0.32 | -75 | -105 | -280 | 0.43 |
| 2022tk | BEAR | **regimeSMA** | 5 | **-70** | 0.40 | -0.43 | 0 | -70 | -95 | 0.31 |

## Read (honest)
- **Core edge is real but conditional: TRENDING bull only.** Both cells earn big in sustained bull
  (6mo, 2024-26) — net>0, PF>1.3, both halves>0, survive 2× cost. That is the whole edge.
- **The gate WORKS as bear protection, not a bear flip.** regime+SMA cuts bear bleed hard (DONCH
  2022tk -534→-13; EMA 2013 -1331→-490) and trims bull profit. It bounds losses; it does not make
  bears positive.
- **Fails the all-weather bar on two counts:** (1) a BULL window LOSES — 2026 Mar-Jun (high-price
  CHOP, not a trend): DONCH -337, EMA -203 even gated. "High price" ≠ "trending". (2) Several bears
  stay net-negative gated (DONCH 2013 -138 / 2015 -384; EMA 2013 -490). Not flat/≥0 across bears.
- **2× cost** wipes the marginal cells (2023-24, weak bears) but the strong bull windows keep a
  comfortable margin.
- 2022(m1, full year) vs 2022tk(tick, Jun-Sep slice) differ because they are DIFFERENT windows +
  granularities, not a contradiction.

## Bottom line
Multi-window evidence CONFIRMS bull beta (was 1 bull + 1 bear; now ~3 bull + 1 mix + 4 bear). It is
NOT an all-weather trend book. **Stays SHADOW.** Re-live only on operator order + a fresh honest cert.
No live wiring change from this grid.
