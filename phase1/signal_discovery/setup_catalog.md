# Signal Discovery -- Setup Catalog

_Generated: 2026-04-29T11:24:01.597735+00:00_

**Corpus:** /sessions/hopeful-friendly-meitner/mnt/omega_repo/phase1/signal_discovery/bars_5s.parquet
**Span:** 2025-04-01 -> 2026-04-01 (~365 days)
**Cost model:** 0.65 pt round-trip (subtracted from every horizon return)
**Gates:** t-stat > 2.0 / freq >= 5.0/day / (win-rate * avg-win) > 1.2 * (loss-rate * avg-loss)

## Per-setup results
### compression_break
- entries total: **18,248**  (freq/day: **49.99**, freq gate: **PASS**)

| horiz | n | mean (pt, net) | std | t-stat | winrate | avg win | avg loss | rr | t/rr gates |
|---|---:|---:|---:|---:|---:|---:|---:|---:|:---:|
|   30s | 18,248 |  -0.658 |  1.371 | -64.82 | 17.1% | +1.014 | -1.003 |  0.21 | fail/fail |
|  2min | 18,248 |  -0.743 |  2.843 | -35.31 | 28.3% | +1.627 | -1.679 |  0.38 | fail/fail |
|  5min | 18,248 |  -0.810 |  4.183 | -26.15 | 34.2% | +2.490 | -2.526 |  0.51 | fail/fail |
| 15min | 18,248 |  -0.743 |  6.973 | -14.40 | 41.1% | +4.115 | -4.132 |  0.69 | fail/fail |
| 30min | 18,248 |  -0.828 |  9.988 | -11.20 | 43.8% | +5.629 | -5.869 |  0.75 | fail/fail |

**Setup verdict:** fail

### spike_reverse
- entries total: **11,014**  (freq/day: **30.18**, freq gate: **PASS**)

| horiz | n | mean (pt, net) | std | t-stat | winrate | avg win | avg loss | rr | t/rr gates |
|---|---:|---:|---:|---:|---:|---:|---:|---:|:---:|
|   30s | 11,014 |  -0.628 |  1.606 | -41.05 | 19.9% | +1.079 | -1.052 |  0.25 | fail/fail |
|  2min | 11,014 |  -0.592 |  2.936 | -21.17 | 32.1% | +1.763 | -1.704 |  0.49 | fail/fail |
|  5min | 11,014 |  -0.612 |  4.467 | -14.39 | 39.0% | +2.460 | -2.577 |  0.61 | fail/fail |
| 15min | 11,014 |  -0.679 |  7.353 |  -9.69 | 43.6% | +4.042 | -4.329 |  0.72 | fail/fail |
| 30min | 11,014 |  -0.667 |  9.924 |  -7.06 | 45.3% | +5.654 | -5.904 |  0.79 | fail/fail |

**Setup verdict:** fail

### momentum_pullback
- entries total: **523,156**  (freq/day: **1433.30**, freq gate: **PASS**)

| horiz | n | mean (pt, net) | std | t-stat | winrate | avg win | avg loss | rr | t/rr gates |
|---|---:|---:|---:|---:|---:|---:|---:|---:|:---:|
|   30s | 523,156 |  -0.651 |  1.431 | -328.93 | 18.7% | +1.013 | -1.032 |  0.23 | fail/fail |
|  2min | 523,156 |  -0.666 |  2.856 | -168.56 | 30.6% | +1.743 | -1.728 |  0.45 | fail/fail |
|  5min | 523,156 |  -0.669 |  4.514 | -107.25 | 36.9% | +2.625 | -2.596 |  0.59 | fail/fail |
| 15min | 523,156 |  -0.692 |  7.665 | -65.31 | 41.7% | +4.425 | -4.346 |  0.73 | fail/fail |
| 30min | 523,156 |  -0.683 | 10.861 | -45.45 | 44.4% | +6.210 | -6.177 |  0.80 | fail/fail |

**Setup verdict:** fail

### level_retest_reject
- entries total: **464,849**  (freq/day: **1273.56**, freq gate: **PASS**)

| horiz | n | mean (pt, net) | std | t-stat | winrate | avg win | avg loss | rr | t/rr gates |
|---|---:|---:|---:|---:|---:|---:|---:|---:|:---:|
|   30s | 464,849 |  -0.637 |  1.514 | -286.97 | 20.7% | +1.032 | -1.073 |  0.25 | fail/fail |
|  2min | 464,849 |  -0.607 |  3.024 | -136.93 | 33.3% | +1.799 | -1.809 |  0.50 | fail/fail |
|  5min | 464,849 |  -0.584 |  4.751 | -83.79 | 39.6% | +2.703 | -2.738 |  0.65 | fail/fail |
| 15min | 464,849 |  -0.582 |  8.045 | -49.29 | 44.4% | +4.503 | -4.647 |  0.77 | fail/fail |
| 30min | 464,849 |  -0.594 | 11.276 | -35.89 | 45.9% | +6.384 | -6.504 |  0.83 | fail/fail |

**Setup verdict:** fail

## Overall
**No setup cleared all three gates.** See CHOSEN_SETUP.md for the negative-result memo and recommended next steps.