# Post-2025-04 Re-validation Report

_Generated: 2026-04-29T11:56:31.522437+00:00_

**Cost model:** 0.05pt commission + 1 * avg_spread per trade
**Period:** 2025-04-01 -> 2026-04-01 (post-microstructure-regime cut)

## Per-cell results

| strategy | tf | dir | n | WR | gross | net | avg/trade | pf | exits |
|---|---|---|---:|---:|---:|---:|---:|---:|:---|
| tsmom | H1 | long | 3,484 | 53.2% | $+20070 | $+17482 | $+5.02 | 1.39 | TIME_EXIT:2769 SL_HIT:715 |
| tsmom | H2 | long | 1,826 | 55.1% | $+14307 | $+12952 | $+7.09 | 1.35 | TIME_EXIT:1452 SL_HIT:374 |
| tsmom | H4 | long | 933 | 61.4% | $+16579 | $+15885 | $+17.03 | 1.66 | TIME_EXIT:761 SL_HIT:172 |
| tsmom | H1 | short | 2,651 | 41.8% | $+4842 | $+2776 | $+1.05 | 1.06 | TIME_EXIT:2162 SL_HIT:489 |
| donchian | H1 | long | 250 | 56.0% | $+3603 | $+3417 | $+13.67 | 1.86 | TIME_EXIT:85 SL_HIT:83 TP_HIT:82 |
| donchian | H4 | long | 79 | 43.0% | $+1225 | $+1168 | $+14.78 | 1.93 | SL_HIT:45 TP_HIT:33 TIME_EXIT:1 |
| donchian | H4 | short | 29 | 41.4% | $+970 | $+946 | $+32.62 | 2.84 | SL_HIT:17 TP_HIT:10 TIME_EXIT:2 |
| donchian | H6 | long | 52 | 53.8% | $+1636 | $+1596 | $+30.69 | 2.94 | TP_HIT:27 SL_HIT:24 TIME_EXIT:1 |
| donchian | H6 | short | 22 | 27.3% | $+147 | $+129 | $+5.87 | 1.19 | SL_HIT:16 TP_HIT:6 |

## Ranked by net (post-cut)

1. **tsmom H1 long** -- n=3,484, net=$+17482, avg=$+5.02, WR 53.2%, pf 1.39
2. **tsmom H4 long** -- n=933, net=$+15885, avg=$+17.03, WR 61.4%, pf 1.66
3. **tsmom H2 long** -- n=1,826, net=$+12952, avg=$+7.09, WR 55.1%, pf 1.35
4. **donchian H1 long** -- n=250, net=$+3417, avg=$+13.67, WR 56.0%, pf 1.86
5. **tsmom H1 short** -- n=2,651, net=$+2776, avg=$+1.05, WR 41.8%, pf 1.06
6. **donchian H6 long** -- n=52, net=$+1596, avg=$+30.69, WR 53.8%, pf 2.94
7. **donchian H4 long** -- n=79, net=$+1168, avg=$+14.78, WR 43.0%, pf 1.93
8. **donchian H4 short** -- n=29, net=$+946, avg=$+32.62, WR 41.4%, pf 2.84
9. **donchian H6 short** -- n=22, net=$+129, avg=$+5.87, WR 27.3%, pf 1.19

## Verdict

**9 of 9 cells profitable in post-2025-04 cut.**
Best: tsmom H1 long at $+17482 net.