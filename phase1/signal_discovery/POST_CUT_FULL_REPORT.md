# Post-2025-04 Re-validation -- ALL Profitable Cells

_Generated: 2026-04-29T12:02:30.498744+00:00_

**Period:** 2025-04-01 -> 2026-04-01 (post-microstructure-regime cut)
**Cost:** 0.05pt commission + 1 * avg_spread (~0.69pt) per trade
**Cells tested:** 32 (all profitable in master_summary.parquet)
**Cells profitable post-cut:** 27 of 32

## Ranked by net pnl (post-cut)

| rank | strategy | tf | dir | family | n | trades/day | WR | net | avg | pf |
|---:|---|---|---|---|---:|---:|---:|---:|---:|---:|
| 1 | tsmom | H1 | long | C | 3,484 | 9.5 | 53.2% | $+17482 | $+5.02 | 1.39 |
| 2 | tsmom | H4 | long | C | 933 | 2.6 | 61.4% | $+15885 | $+17.03 | 1.66 |
| 3 | tsmom | H6 | long | C | 661 | 1.8 | 57.8% | $+13380 | $+20.24 | 1.65 |
| 4 | tsmom | H2 | long | C | 1,826 | 5.0 | 55.1% | $+12952 | $+7.09 | 1.35 |
| 5 | tsmom | D1 | long | C | 216 | 0.6 | 56.5% | $+9109 | $+42.17 | 1.65 |
| 6 | donchian | H1 | long | A | 250 | 0.7 | 56.0% | $+3417 | $+13.67 | 1.86 |
| 7 | donchian | H6 | long | A | 52 | 0.1 | 53.8% | $+1596 | $+30.69 | 2.94 |
| 8 | ema_pullback | H6 | long | A | 75 | 0.2 | 48.0% | $+1411 | $+18.81 | 1.90 |
| 9 | donchian | H4 | long | A | 79 | 0.2 | 43.0% | $+1168 | $+14.78 | 1.93 |
| 10 | ema_pullback | H2 | long | A | 212 | 0.6 | 38.2% | $+962 | $+4.54 | 1.31 |
| 11 | donchian | H4 | short | A | 29 | 0.1 | 41.4% | $+946 | $+32.62 | 2.84 |
| 12 | ema_pullback | H1 | long | A | 398 | 1.1 | 33.7% | $+894 | $+2.25 | 1.22 |
| 13 | donchian | D1 | long | A | 16 | 0.0 | 56.2% | $+862 | $+53.86 | 3.17 |
| 14 | bollinger | H4 | long | B-BB | 47 | 0.1 | 66.0% | $+840 | $+17.88 | 1.69 |
| 15 | ema_pullback | H4 | long | A | 111 | 0.3 | 40.5% | $+739 | $+6.66 | 1.34 |
| 16 | donchian | H2 | long | A | 127 | 0.3 | 40.2% | $+692 | $+5.45 | 1.40 |
| 17 | bollinger | D1 | short | B-BB | 18 | 0.0 | 33.3% | $+589 | $+32.72 | 1.57 |
| 18 | bollinger | H6 | long | B-BB | 29 | 0.1 | 55.2% | $+527 | $+18.16 | 1.49 |
| 19 | asian_break | H1 | short | D | 129 | 0.4 | 32.6% | $+365 | $+2.83 | 1.31 |
| 20 | rsi_revert | D1 | long | B-RSI | 3 | 0.0 | 66.7% | $+317 | $+105.61 | 2.44 |
| 21 | asian_break | M15 | short | D | 355 | 1.0 | 26.2% | $+314 | $+0.89 | 1.12 |
| 22 | rsi_revert | H1 | long | B-RSI | 146 | 0.4 | 43.2% | $+297 | $+2.04 | 1.09 |
| 23 | donchian | D1 | short | A | 3 | 0.0 | 33.3% | $+227 | $+75.81 | 3.00 |
| 24 | bollinger | H2 | long | B-BB | 98 | 0.3 | 55.1% | $+173 | $+1.77 | 1.08 |
| 25 | asian_break | H2 | short | D | 88 | 0.2 | 36.4% | $+171 | $+1.95 | 1.17 |
| 26 | rsi_revert | H2 | long | B-RSI | 59 | 0.2 | 42.4% | $+158 | $+2.68 | 1.09 |
| 27 | donchian | H6 | short | A | 22 | 0.1 | 27.3% | $+129 | $+5.87 | 1.19 |
| 28 | rsi_revert | H6 | long | B-RSI | 16 | 0.0 | 43.8% | $-57 | $-3.54 | 0.93 |
| 29 | rsi_revert | H4 | long | B-RSI | 32 | 0.1 | 34.4% | $-209 | $-6.54 | 0.84 |
| 30 | rsi_revert | M15 | long | B-RSI | 565 | 1.5 | 44.6% | $-249 | $-0.44 | 0.96 |
| 31 | bollinger | D1 | long | B-BB | 3 | 0.0 | 33.3% | $-323 | $-107.67 | 0.24 |
| 32 | rsi_revert | D1 | short | B-RSI | 44 | 0.1 | 15.9% | $-1161 | $-26.39 | 0.64 |

## Cells that LOST in post-cut

- **rsi_revert H6 long** -- n=16, net=$-57 -- DEPRECATE
- **rsi_revert H4 long** -- n=32, net=$-209 -- DEPRECATE
- **rsi_revert M15 long** -- n=565, net=$-249 -- DEPRECATE
- **bollinger D1 long** -- n=3, net=$-323 -- DEPRECATE
- **rsi_revert D1 short** -- n=44, net=$-1161 -- DEPRECATE

## Combined portfolio if all profitable cells deployed

- **Total trades:** 9,466 (25.9/day)
- **Total net pnl:** $+85,601 on 1 unit over 365 days
- **Bidirectional cells:** 7 short + 20 long
