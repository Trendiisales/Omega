# Edge Discovery 2026-05-29

## Mandate
Find edges, validate on backtest data, build a daily-trading system on all
symbols, net-positive after costs.

## Method
1. Extended `backtest/multi_tf_sweep.cpp`: per-symbol cost from
   `OmegaCostGuard::estimated_cost_usd`, `--from-unix`/`--to-unix` IS/OOS
   slicing, +7 symbols added to baselines (AUD, NZD, CAD, EURGBP, GER40,
   BCOUSD, DJ30).
2. Full sweep: 12 symbols × {5m, 15m, 30m, 1h, 4h} × 6 families
   (Donchian, BollingerMR, RSI_Extreme, MACrossover, MomentumN, ZScoreMR)
   = 900 cells. Source: `/Users/jo/Tick/` 13-26 mo per symbol.
3. S43 IS gate: Sharpe ≥ 0.5, net > 0, n ≥ 30. **42 passed.**
4. Walk-forward 70/30 IS/OOS split per symbol (unix-ts at first+0.7×span).
   OOS gate: Sharpe ≥ 0.3, net > 0, n ≥ 10.
   **14 passed both. 12 are sign-stable IS↔OOS mean_r.**

Scripts:
- `scripts/discovery_sweep_2026-05-29.sh` — full IS+OOS sweep
- `scripts/oos_verify_2026-05-29.sh` — per-symbol 70/30 split, parallel
- `scripts/wf_filter_2026-05-29.py` — join + filter + robust subset

Outputs: `outputs/discovery_2026-05-29/`
- `leaderboard_full.csv` (900 cells, full window)
- `leaderboard_is.csv`   (900 cells, IS 70%)
- `leaderboard_oos.csv`  (900 cells, OOS 30%)
- `survivors_s43.csv`    (42 cells passing IS gate)
- `wf_survivors.csv`     (14 cells passing IS+OOS gates)
- `wf_robust.csv`        (12 cells, sign-stable)

## The 12 Robust Survivors

Ranked by full-window Sharpe. Costs subtracted per-symbol per OmegaCostGuard.

| # | Sym | TF | Engine | Params | IS Sh | OOS Sh | Full Sh | Full Net$ | trades/day |
|---|---|---|---|---|---|---|---|---|---|
| 1 | XAUUSD | 4h | DonchianBreakout | N=20 | 2.53 | 2.01 | **2.93** | $2,483 | 0.19 |
| 2 | XAUUSD | 4h | DonchianBreakout | N=100 | 0.74 | 2.36 | 2.49 | $1,276 | 0.10 |
| 3 | GER40 | 4h | RSI_Extreme | N=7/lo=30/hi=70 | 1.81 | 0.91 | 2.17 | $4,692 | 0.28 |
| 4 | GER40 | 15m | MACrossover | fast=10/slow=30 | 1.42 | 1.61 | 1.96 | $3,866 | 1.64 |
| 5 | GER40 | 1h | RSI_Extreme | N=14/lo=30/hi=70 | 1.77 | 0.87 | 1.90 | $2,989 | 0.50 |
| 6 | GER40 | 1h | DonchianBreakout | N=100 | 1.63 | 0.50 | 1.72 | $2,991 | 0.30 |
| 7 | XAUUSD | 4h | MACrossover | fast=10/slow=30 | 1.78 | 0.57 | 1.21 | $938 | 0.12 |
| 8 | USTEC | 4h | RSI_Extreme | N=7/lo=30/hi=70 | 0.75 | 1.00 | 1.13 | $3,556 | 0.43 |
| 9 | GER40 | 15m | MACrossover | fast=20/slow=50 | 0.62 | 1.07 | 0.99 | $1,618 | 1.18 |
| 10 | USTEC | 4h | ZScoreMR | W=20/Z=2.5 | 0.50 | 0.86 | 0.90 | $1,782 | 0.14 |
| 11 | GER40 | 5m | RSI_Extreme | N=14/lo=30/hi=70 | 0.53 | 0.73 | 0.79 | $1,099 | 4.68 |
| 12 | GER40 | 30m | RSI_Extreme | N=14/lo=20/hi=80 | 0.54 | 0.56 | 0.72 | $588 | 0.19 |

**Portfolio:**
- Symbols: XAUUSD (4 cells), GER40 (7 cells), USTEC (2 cells, alias NSXUSD)
- Engines: Donchian (4), RSI_Extreme (5), MACrossover (2), ZScoreMR (1)
- Timeframes: 4h (7), 1h (2), 15m (2), 5m (1), 30m (1)
- Estimated raw daily trade volume: **9.7 trades/day**
- Post-overlap dedup estimate: ~7-8 trades/day

## Symbols With Zero Survivors
EURUSD, GBPUSD, USDJPY, AUDUSD, NZDUSD, USDCAD, EURGBP, BCOUSD, US500/SPXUSD,
DJ30. Tested families (Donch/BollMR/RSI/MACross/Mom/ZScore) found no edge that
survives cost + walk-forward. Options:
- Different families: session-bias (London open break, NY range fade), carry,
  micro-mean-reversion at M1/M5 with absolute spread cap
- Different timeframes: M1/M5 scalp tier
- Different cost model: variable spread with median per-session

User selected: **retry FX with session-bias + carry families.** Queued for
next sweep.

## Wire Plan (Existing → New Engines)

| Survivor | Existing engine | New code needed |
|---|---|---|
| XAUUSD 4h Donch N=20 | `g_donchian` H4_long cell (period=20 default) | None — already running shadow |
| XAUUSD 4h Donch N=100 | None | Add 2nd H4 cell w/ period=100 to DonchianPortfolio |
| GER40 4h RSI N=7 | None | New `g_ger40_rsi` (clone RSIExtremeTurn for GER40) |
| GER40 15m MACross 10/30 | None | New `g_ger40_mac` engine + 15m bar dispatch |
| GER40 1h RSI N=14 | Same as #3, different cell | + 1h cell to `g_ger40_rsi` |
| GER40 1h Donch N=100 | None | New `g_ger40_donchian` (clone DonchianPortfolio) |
| XAUUSD 4h MACross 10/30 | `g_xau_tf_4h` EmaCross8_21 cell (bit 6) | Change bit 6 cell to 10/30, OR new cell |
| USTEC 4h RSI N=7 | None | New `g_ustec_rsi` |
| GER40 15m MACross 20/50 | Same as #4 | + 2nd cell w/ 20/50 |
| USTEC 4h ZScoreMR W=20 | None | New `g_ustec_zscore` or reuse BollingerMR |
| GER40 5m RSI N=14 | Same engine as #3 | + 5m cell |
| GER40 30m RSI N=14/20-80 | Same engine as #3 | + 30m cell |

**Net new engine instances:** 4
- `g_ger40_rsi` (5m, 30m, 1h, 4h cells)
- `g_ger40_mac` (15m two cells)
- `g_ger40_donchian` (1h cell)
- `g_ustec_rsi`, `g_ustec_zscore` (4h cells each)

**Per-engine effort:** clone existing class, change symbol const, add to
`engine_init.hpp` w/ `shadow_mode=true` + seed CSV path, add tick dispatch in
`tick_indices.hpp`. ~30-45 min per engine × 4 = 2-3h.

## Caveats
- Touch-fill: harness fills at TP/SL level minus half-spread. Real broker
  partial fills + slippage can cut Sharpe 30-50%. Live shadow validation
  required.
- XAU 2yr window 2024-03 → 2026-04 is mostly bull. Long-bias Donchian
  amplified by trend. OOS window 2026-01 → 2026-04 also bull. Re-test
  on the next bear leg.
- GER40 = 8/12 cells. Concentration risk on single-index regime change.
- USTEC backtest data is NSXUSD historical; live broker symbol is USTEC.F
  or NAS100. Symbol mapping at wire time matters for tick subscription.
- All survivors passed `OmegaCostGuard` estimated cost (spread + slip + comm).
  Live spread can spike 2-5× at news/Asian-thin/rollover — already gated by
  `RelativeSpreadGate` and `OmegaCostGuard::is_viable` at fire time.

## Promotion Gate (Operator-Selected)
Per CLAUDE.md standard: **30 shadow trades + WR ≥ 35% + net positive.**

Per survivor expected wallclock to gate:
- GER40 5m RSI N=14: ~6 trading days (4.7/day → 30 trades)
- GER40 15m MACross 10/30: ~18 days
- USTEC 4h RSI: ~70 days
- XAUUSD 4h Donch N=20: ~150 days

→ M-tier engines clear in a week; H-tier engines need months. Consider
relaxing to 20 trades for slow-TF engines to match the bar cadence.

## Next-Session Sequence
1. Wire top-4 survivors first (Sharpe ≥ 1.9): XAU 4h Donch N=20 (already
   present, no-op verify), GER40 4h RSI N=7, GER40 15m MACross 10/30,
   GER40 1h RSI N=14. ~3h.
2. Re-run FX sweep with session-bias + carry families. ~1h.
3. Wire remaining 8 survivors after top-4 validate live tick path. ~4h.
4. Monitor for first shadow signals. Promotion review at 30 trades each.
