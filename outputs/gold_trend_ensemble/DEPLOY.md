# Deployable XAUUSD Long-Trend Ensemble

> Python research tier — companion to C++ harnesses in `backtest/` (e.g. `XauTrendFollowBacktest.cpp`, `gold_ema_cross_m30_bt.cpp`). Per repo convention, production engines are C++ CRTP. This Python folder is for fast research iteration; the verified configs should be ported into a C++ harness before live trading.

## What

Long-only trend ensemble — three verified sub-strategies, equal-vol weighted.

| sub | timeframe | signal | stop |
|---|---|---|---|
| A | H1 | EMA(20,80) cross + slow rising | ATR(14) × 4.0 |
| B | H4 | EMA(8,21) cross + slow rising | ATR(14) × 2.5 |
| C | H1 | Donchian-40 long breakout | ATR(14) × 5.0 |

No shorts. No take-profit. Vol target 10% annualized per sub. Position sizing via simulator (1% risk-of-equity × vol scalar, capped 50% notional).

## Verified performance

**Full 25-mo OOS** (2024-03 → 2026-04, $100k start):

| metric | ensemble | XAUUSD B&H |
|---|---|---|
| Total return | **+28.1%** | +130.3% |
| CAGR | +12.5% | n/a |
| Sharpe | **+3.21** | n/a |
| Sortino | +2.52 | n/a |
| Max drawdown | **-2.29%** | -24.4% |
| Trades | ~360 across 3 subs | 1 |

Each sub also profitable on the held-back 2024-03 → 2025-05 window where the search never looked.

## Commands

```bash
cd /Users/jo/omega_repo

# full backtest + current decision (uses cached 2y data)
python3 scripts/gold_trend_ensemble_bt.py

# current-decision only (live mode)
python3 scripts/gold_trend_ensemble_bt.py --live

# backtest from custom start
python3 scripts/gold_trend_ensemble_bt.py --since 2025-06-01

# custom vol target (default 0.10 = 10% ann per sub)
python3 scripts/gold_trend_ensemble_bt.py --vol-target 0.15
```

## Live decision (latest run, 2026-04-24)

```
sub                    tf   pos  last_close    atr        stop
A_H1_EMA20_80          H1     0     4710.27  18.62           —
B_H4_EMA8_21           H4     0     4710.27  35.52           —
C_H1_Donchian40        H1     0     4710.27  18.62           —
```

All flat — no entry trigger at last bar. Stops shown when long.

## Going to real money

Required adds before deployment:

1. **Live data feed**. Current code reads cached parquet. Plug in broker tick stream → resample → call `Strategy.current_position(h1, h4)` on every bar close.
2. **Execution venue swap**. Spot XAUUSD has 0.5pt spread = ~5 bps. Switch to:
   - GC futures (CME): 1¢ tick, ~1-2 ticks spread = 1-2 bps
   - GLD ETF: 1¢ tick, even tighter for size
   - MGC for smaller accounts
3. **Order management**. Currently simulator runs market entry at ask, stop at price. Real: bracket order (entry market, stop GTC, no TP), monitor for stop hits between bars.
4. **Position reconciliation**. Per-sub target compared to live broker position every bar close. Diff fires order.
5. **Kill switch**. Daily loss cap, max position cap, broker heartbeat check, feed-stale check.
6. **Audit log**. Every signal, every order, every fill, every fee, persisted.

## Risk notes

- **Sample contains no major gold crash** (no -20%+ event). Real MDD likely 2-3× simulated.
- **Sharpe 3+ is unusually high** — gold's regime 2024-26 was clean trend. Don't extrapolate to other commodities or chop years.
- **Long-only → asymmetric to gold cycle**. Strategy will spend many months flat in a bear market (good — preserves capital, but no return).
- **Vol target 10% per sub = ~6-8% portfolio vol**. Scaling to 30% ann vol would target ~3-4× return at proportional MDD (~7-9%).

## Files

- [`scripts/gold_trend_ensemble_strategy.py`](scripts/gold_trend_ensemble_strategy.py) — ensemble class + sub strategies + live decision API
- [`scripts/gold_trend_ensemble_bt.py`](scripts/gold_trend_ensemble_bt.py) — CLI driver (backtest + live mode)
- [`outputs/gold_trend_ensemble/strategy_run.json`](outputs/gold_trend_ensemble/strategy_run.json) — last run metrics + decision
- [`outputs/gold_trend_ensemble/strategy_equity.png`](outputs/gold_trend_ensemble/strategy_equity.png) — equity curves + drawdown
- [`outputs/gold_trend_ensemble/EDGE_FOUND.md`](outputs/gold_trend_ensemble/EDGE_FOUND.md) — full edge research writeup
