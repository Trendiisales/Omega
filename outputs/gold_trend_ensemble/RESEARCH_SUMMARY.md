# Gold Engine — Build & Test Summary

Source brief: [`deep-research-report.md`](../../Downloads/deep-research-report.md) (modular multi-sleeve gold engine).

## What was built

`/Users/jo/omega_repo/` — a working, modular Python research engine matching the architecture described in the brief, scaled down to what spot XAUUSD tick data supports:

| File | Purpose |
| --- | --- |
| `src/loader.py` | Loads 14-day L2-CSV ticks, builds 5m/15m/H1 bars with pre-engineered features (l2_imb, vpin, ewm_drift, regime). Cleans watchdog-dead/crossed-quote ticks. |
| `src/loader_long.py` | Streams 4.6 GB / 154 M-row Duka XAUUSD tick file (2024-03 → 2026-04) in chunks → 15m bars. |
| `src/sleeves.py` | Four sleeves: **trend** (multi-horizon EMA + slope-z + vol filter), **pullback** (London/NY session-gated EMA pullback), **meanrev** (VWAP z-fade, regime + drift gated), **micro** (l2_imb + ewm_drift + vpin). |
| `src/portfolio.py` | Vol-target per-sleeve sizing, ATR-based stops/takes, bid/ask execution from real tick spreads, weighted combination + trade aggregation. |
| `src/metrics.py` | Sharpe, Sortino, max DD, CAGR, hit, payoff, Deflated Sharpe (no SciPy dep — Beasley-Springer-Moro normal-cdf). |
| `src/run.py` | Short-window driver: 5m + 15m sleeves on the L2 dataset, single 75/25 hold-out. |
| `src/run_long.py` | Long-window driver: 22-fold walk-forward (4-mo train / 1-mo test, rolling) on 2y data. |

## Architecture mapping to the report

| Report layer | Implementation |
| --- | --- |
| Contract-true cost-complete sim | ✅ bid/ask exec, ATR stops, mark-to-mid, real per-tick spreads. |
| Walk-forward, rolling, frozen OOS | ✅ `run_long.py` — 22 folds, parameters frozen inside each test window. |
| Multi-sleeve / risk allocation | ✅ inverse-vol weights with optional train-PnL gate, 0.5 cap per sleeve. |
| Vol-managed sizing | ✅ per-sleeve 8% annualized vol target × 1% risk-of-equity. |
| Anti-overfit metrics | ✅ Deflated Sharpe Ratio implemented (n_trials, skew, kurt). |
| Session breakdown / event awareness | ✅ Asia / London / NY-overlap / NY-late tagging on every bar. |
| COMEX futures + options + macro + sentiment | ❌ no such data available locally; sleeves use spot tape + pre-engineered features only. |

## Data assessment

| Dataset | Rows | Live coverage | Notes |
| --- | --- | --- | --- |
| `Downloads/l2_ticks_XAUUSD_2026-05-*.csv` | 3.45 M raw / 1.88 M alive | May 8 + May 11–15 (6 dense days) | Despite "L2" name, `depth_*` and `l2_*_vol` columns are all zero; only `l2_imb`, `vpin`, `ewm_drift`, `regime`, `micro_edge` carry signal. Watchdog-dead drops ~54%. |
| `Tick/duka_ticks/XAUUSD_2024-03_2026-04_combined.csv` | 154 M | 26 months continuous | No L2 features (plain `timestamp, ask, bid`). Used for the long walk-forward. |

## Headline results

### Long-window walk-forward (15m bars, 22 OOS months)

| Variant | OOS PnL | OOS return | Sharpe | Max DD | Trades |
| --- | --- | --- | --- | --- | --- |
| Gated (train-Sharpe filter) | **-$4,917** | -4.92% | -1.43 | -5.66% | ~1k |
| Always-on (inverse-vol all 3 sleeves) | **-$5,565** | -5.56% | -2.28 | -6.83% | ~4k |
| **Buy & hold XAUUSD (same window)** | **+$102,210** | **+102.21%** | n/a | -26.05% | 1 |

The sleeves as parameterized **underperform a long-only gold benchmark by ~$107 k**. Per-fold detail is in `RESULTS_LONG.md`. Drawdowns are small (good), but the engine is essentially extracting noise — it does not capture gold's primary 2024-26 trend.

### Short-window hold-out (5m bars, May 2026 L2)

After train-Sharpe gating: portfolio runs the single sleeve that survived (`trend` on 5m, `meanrev` on 15m). Both produced negative OOS returns over the 1.5-day hold-out (data window too small to draw conclusions). See `RESULTS.md`.

## Diagnosis — why the sleeves underperform

1. **Long/short symmetric trend got chopped.** Gold trended hard up; trend sleeve's short signals lost into the persistent rally.
2. **Mean-reversion in a trending tape pays a tax.** Regime/drift filters were too permissive — VWAP fades got stopped on continuation.
3. **No macro conditioning.** Report calls for USD / real-yields / CVOL / event filters. None available locally → sleeves run blind to regime drivers.
4. **Spread friction at high trade count.** ~200 trades/mo × $0.51 mean spread × bid/ask round-trips eats real edge fast.
5. **ATR stops in 15m gold noise.** 2.5× ATR exit gets visited often during normal range expansion → low hit, lower payoff than the trend literature implies.
6. **Selector bias.** Both gated and always-on weighting produced losses, but always-on was slightly worse — small evidence the train signal does have some forecasting value, but it's drowned by the misspecified universe.

## What this validates

- The **infrastructure** works: tick → bar → sleeve → vol-targeted portfolio → walk-forward → metrics → equity curve → reports. ~750 lines of Python.
- The **cost model is realistic** (per-tick spreads, bid/ask fills).
- The **honest result** is exactly what the report's Validation section warns about: simple sleeves on a limited data universe with no macro conditioning do not generate gold alpha. The right next step is **broaden the data universe**, not over-tune these parameters.

## Recommended next moves (in build-order)

1. **Add COMEX `GC` / `MGC` contract-level data** (CME DataMine settlements). Switch execution backbone from spot to GC1/MGC1 — the report's primary recommendation.
2. **Build the macro conditioning layer** (FRED real yields, DXY, CVOL, GVZ, FOMC/CPI/NFP calendar). Gate every sleeve on regime + event blackouts.
3. **Add a long-bias** to trend (or a market-neutral hedge) — pure trend on a structurally-rising commodity needs asymmetric treatment.
4. **Parameter sweep with multiple-testing correction.** Use the existing walk-forward harness + White's Reality Check / Deflated SR (already implemented) to bound search.
5. **Layer in options sleeve** for event convexity. Requires OPRA GLD options chain or CME OG.
6. **Don't add ML / sentiment yet** — fix the universe first. Per report: "ML first for regime classification and sizing, not unrestricted directional prediction."

## Quick rerun

```bash
cd /Users/jo/omega_repo
python3 src/run.py            # short-window L2 backtest (~10 sec)
python3 src/run_long.py       # 22-fold walk-forward on 2y ticks (~90 sec, first run ~3 min for cache)
WEIGHT_MODE=gated python3 src/run_long.py   # use train-Sharpe gate instead
```

Outputs land in `/Users/jo/omega_repo/reports/`: `results.json`, `results_long.json`, `RESULTS.md`, `RESULTS_LONG.md`, `equity.png`, `oos_equity_long.png`.
