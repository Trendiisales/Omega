# Replay test: MinimalH4US30Breakout

Drives the **production engine** (`include/MinimalH4US30Breakout.hpp`) through
2yr Tickstory DJ30.F tick data and verifies the engine reproduces strategy
behaviour consistent with the original sweep harness `htf_bt_multi.cpp`.

## Why this matters

The sweep harness and the production engine implement the same strategy but
with different execution models:

| | Sweep harness | Production engine |
|---|---|---|
| Bars built from | Mid (with intrabar bid/ask hi/lo) | Mid (engine-internal) |
| Entry price | `bar_close ± half_spread` | Live `ask` / `bid` at tick |
| Exit price | Exact SL / TP price | Live `bid` / `ask` when crossed |
| ATR | Wilder, SMA seed | Wilder, SMA seed (matches) |
| Donchian | Prior N bars | Prior N bars (matches) |

If the engine produces ~184 trades within ~15% on the same data the sweep used,
the strategy logic is faithful. PnL difference is then attributable to
execution model only (live tick fills vs synthetic SL/TP fills).

## Build

```
cd backtest
clang++ -O3 -std=c++17 -I../include -o replay_minimal_us30 replay_minimal_us30.cpp
```

(g++ also works.)

## Run

```
./replay_minimal_us30 "/Users/jo/Library/CloudStorage/GoogleDrive-kiwi18@gmail.com/My Drive/Tickstory/US30/dow30_2yr.csv"
```

Expect ~30-60s runtime on 21M ticks (Mac M-series).

## Pass thresholds

Compared against sweep result for D=10 SL=1.0 TP=4.0:

| Metric | Sweep | Pass range |
|---|---|---|
| Trade count | 184 | 156 - 211 (±15%) |
| Total PnL | +$637 | +$318 - +$955 (±50%) |
| Profit factor | 1.54 | ≥ 1.20 |
| Max drawdown | -$330 | ≤ -$660 (2x slack) |

PnL band is loose because tick-realistic fills will diverge from SL/TP-exact
fills. What matters most for engine correctness is trade count and PF — those
prove the signal generation is faithful to the strategy.

Exit code `0` = all pass, `2` = any threshold failed.

## Outputs

- `replay_us30_trades.csv` — full trade list with timestamps, entry/exit,
  SL/TP, size, PnL, exit reason
- `replay_us30_summary.txt` — summary stats + threshold pass/fail breakdown

## Interpreting results

| Outcome | Diagnosis |
|---|---|
| All PASS | Engine matches strategy. Ship to VPS for live shadow validation. |
| Trade count off by >15% | Signal logic mismatch — investigate Donchian / ATR seeding |
| Trade count OK, PnL way off | Sizing or PnL math bug (check `dollars_per_point`) |
| Trade count OK, many "OTHER" exits | Timeout / weekend close firing too often |
| Engine 0 trades, ATR never seeded | First H4 bar boundary detection broken |

If anything fails, paste the summary block back to next session and we
diagnose from `replay_us30_trades.csv`.
