# L2-Native Discovery Verdict 2026-05-30

## Mandate
Find net-positive edges using L2 microprice + imbalance as PRIMARY signal
(not just as a gate on top of bar engines).

## Method
- `backtest/l2_discovery.cpp` -- tick-level harness over L2 csv data.
- Symbols: XAUUSD, US500, USTEC (7d each from VPS, 21 files, ~2 GB).
- Families:
  - MicroMomentum  -- mic_avg(N) > thr -> follow
  - MicroZ_follow  -- mic z-score > z_thr -> follow
  - MicroZ_fade    -- mic z-score > z_thr -> fade
  - ImbalanceFade  -- imb > hi_thr -> short; < lo_thr -> long
- Per-symbol cost from OmegaCostGuard production values.
- Bracket: fixed-pt SL/TP sized so gross TP >= 3x cost. 1200-tick hold.

## Result -- ALL families fail across all 3 symbols.

USTEC (most-varied L2 data) top cells by Sharpe:

| Family | Params | n | WR | Net$ | Sharpe |
|---|---|---|---|---|---|
| MicroMomentum  | N=300;thr=0.2  | 116  | 1.72% | -$460   | -41 |
| MicroZ_follow  | N=30;z=3       | 183  | 1.09% | -$734   | -65 |
| MicroZ_follow  | N=300;z=2      | 1627 | 0.37% | -$6,623 | -338 |
| MicroZ_fade    | N=100;z=2      | 2172 | 0.37% | -$8,841 | -391 |

Win rates 0.1-1.7% across 100+ tested cells. Random direction on 3:1
R:R bracket = ~25% WR. So these signals are STRONGLY ANTI-EDGE at
the tick level -- 98%+ of entries get hit at SL before reaching TP.

XAUUSD: only ImbalanceFade fires (XAU micro_edge column is all-zeros
in CSV per memory:feedback-l2-data-quality). ImbalanceFade also 0-2%
WR.

US500: similar pattern.

## Root cause -- cost-to-signal ratio at retail spread

L2 microprice flips have predictive horizon ~5-15 ticks of mid follow-
through, roughly 5-50 cents on indices, 1-3 pts on XAU. Retail cost:
- XAUUSD: $0.66 rt -> need 66 cents move at 0.01 lot = 66 cents at TP
- US500:  $2.00 rt -> need $20 move at 0.10 lot = 0.4 SP pts
- USTEC:  $1.10 rt -> need $11 move at 0.10 lot = 1.1 USTEC pts

The bracket TP that beats cost is MUCH WIDER than typical L2-signal
follow-through. So most trades hit SL before TP. Even when the signal
is directionally right, the bracket geometry forced by cost makes it
unprofitable.

## Implication -- L2 as gate only

The earlier L2 work (IndexSwingEngine commit c839da58: 30-tick rolling
gate on entry; commit f6a1926c: pending-delay + sizing + L2-trail) uses
L2 as a CONFIRMATION FILTER on top of bar-based signals. That works
because:

- The bar-based signal (H1+H4 EMA cross) carries the longer-horizon
  edge (~$5-50 expected move) that COVERS the round-trip cost.
- L2 is layered on top to:
  - Block opposed-flow entries (gate)
  - Delay entry until flow confirms (pending)
  - Scale lot by conviction (sizing)
  - Exit early when flow flips (L2-trail)

L2 as primary signal does NOT have a horizon long enough to cover
retail cost. Conclusion: keep L2 as gate; do not pursue L2-native
strategies at retail cost structure.

## Files
- backtest/l2_discovery.cpp (NEW) -- sweep harness
- /tmp/l2disc_USTEC.csv etc. -- per-symbol full sweep results
- HANDOFF_S38_L2_DISCOVERY_2026-05-30.md (this doc)

## What WOULD make L2-native viable (future, not now)
- Institutional spread (0.1-0.2 pt cost) -- not retail
- HFT colocation latency -- not VPS-broker model
- IBKR Pro Direct Market Access if available end-month, with raw
  spread + per-share commission instead of all-in spread
- Or: hold longer than tick-window (use micro to time entry on
  bar-based strategy, which is what we already do)
