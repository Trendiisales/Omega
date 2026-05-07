# NR2-NR20 Viability Check — Run Instructions

These three files are the viability gate for adding a 6th open-position source
to Omega based on Zeiierman's "Smart NR2-NR20 and Inside Bar" idea. **No core
Omega code is touched.** All work is in standalone Python.

## What's in this drop

| File | Purpose |
|---|---|
| `HANDOFF_NR2_20_BACKTEST.md` | Full spec — rules, params, walk-forward windows, acceptance criteria. Read first. |
| `nr2_20_backtest.py` | Single-run NR2-20 backtest. Self-test mode included. Now supports `--trend-filter {ema,vwap,both}`. |
| `wf_nr2_20.py` | Walk-forward driver for NR2-20 only. |
| `vwap.py` | Shared VWAP + HTF-EMA utilities. Imported by both strategies. |
| `vwap_continuation_backtest.py` | Standalone VWAP-continuation strategy. Self-test mode included. |
| `wf_compare.py` | Walk-forward harness that runs **both** strategies head-to-head and reports correlation. **This is the head-to-head viability gate.** |
| `README_NR2_20.md` | This file. |

## Step 0 — Sanity check the engine

Before pointing at real data, confirm the code runs and the synthetic LONG
setup produces the expected trade:

```bash
cd ~/omega_repo/backtest/nr2_20
python3 nr2_20_backtest.py --self-test
```

Expected output ends with:

```
PASS: synthetic LONG setup produced expected trade
```

If it fails, **stop** and reply with the printed assertion failures —
I built this without being able to run anything in the session sandbox
(Linux worker was OOD with `No space left on device`), so all execution-side
verification falls to you.

## Step 1 — Convert HISTDATA bars to the expected CSV schema

The script expects a CSV with header:

```
ts,open,high,low,close,volume
```

`ts` can be ISO-8601 (`2024-01-01T00:00:00Z`) or HISTDATA-native
(`20240101 000000`) — both parsers are wired in. Volume is optional.

If your existing FVG pipeline already emits a 15m XAUUSD CSV (the session
summary references `XAUUSD_15min_top10_be0.0_sl2.5_tp5.0_wf2025-12-01/`), just
extract the OHLC columns from whatever feeds that. Otherwise:

```bash
# Example: HISTDATA M1 → 15m via your existing tooling, then point at it
python3 nr2_20_backtest.py --bars /path/to/XAUUSD_15min.csv --out trades.csv
```

You'll see a load message like:

```
Loading bars from /path/to/XAUUSD_15min.csv ...
  loaded 67200 bars (2024-01-01T00:00:00+00:00 → 2026-01-01T00:00:00+00:00)
```

then the trade summary.

## Step 2 — Single-window run with defaults

This gives you a feel for whether the strategy emits anything sane on your
data before you spend cycles on walk-forward:

```bash
python3 nr2_20_backtest.py \
    --bars /path/to/XAUUSD_15min.csv \
    --out nr2_20_trades_default.csv
```

Look at:
- `trades` — is the strategy active? Should see >=  hundreds on 2 years of data.
- `boxes_formed` vs `trades` — what fraction of boxes generate entries?
- `boxes_expired_unused` — many expirations means box_expiry may be too short
  or the trend filter is too restrictive.
- `entries_rejected_trend` — wrong-direction breakouts. High count is *good*
  (filter is doing work).
- `profit_factor` and `total_R` — first read on standalone profitability.
  Don't make decisions on this; walk-forward is the real gate.

Open `nr2_20_trades_default.csv` in your tool of choice — schema in
`HANDOFF_NR2_20_BACKTEST.md` §12.

## Step 3 — Walk-forward (the actual decision)

```bash
python3 wf_nr2_20.py \
    --bars /path/to/XAUUSD_15min.csv \
    --samples 200
```

This runs random sample of 200 param combos per train window, picks top-10 by
combined score, runs them on the held-out test segment, then aggregates across
all windows.

Expect this to take **5–30 minutes** on a typical Mac depending on bar count.
Per-window progress prints to stderr.

For a faster smoke check (will produce noisier results):

```bash
python3 wf_nr2_20.py \
    --bars /path/to/XAUUSD_15min.csv \
    --samples 50 \
    --max-windows 3
```

For the full gate (slow, more credible):

```bash
python3 wf_nr2_20.py \
    --bars /path/to/XAUUSD_15min.csv \
    --samples 0           # exhaustive grid, ~8748 combos × N windows
```

## Step 4 — Read the verdict

The script prints a final block like:

```
======================================================================
ACCEPTANCE GATE (HANDOFF_NR2_20_BACKTEST.md §10)
======================================================================
  profit_factor        1.4523 >= 1.3   ->  PASS
  sharpe_trade         1.1240 >= 1.0   ->  PASS
  max_dd_R             8.7100 <= 15.0  ->  PASS
  trades               412 >= 200      ->  PASS
  win_rate             0.4187 >= 0.38  ->  PASS

RESULT: PASS — proceed to C++ port (XauusdNr2_20Engine.hpp)
```

Or:

```
RESULT: FAIL — do NOT proceed to C++. Investigate failed metrics, ...
```

The script exit code mirrors this: `0` = pass, `1` = fail, `2` = bad input.

Two artefact CSVs are written:
- `wf_nr2_20_results.csv` — every train + test run. Useful for grepping
  parameter behavior.
- `wf_nr2_20_top.csv` — top-K test runs per window. Use this to check
  parameter stability (do the same `min_nr` / `trend_ma_period` keep showing
  up across windows? If not, it's curve-fit).

## Step 5 — What to send back

Whatever the result, paste back:
1. The full `ACCEPTANCE GATE` block.
2. The aggregate metrics block above it.
3. `wf_nr2_20_top.csv` (or its first ~30 rows if huge).

Then we decide:
- **PASS** → write the spec for `XauusdNr2_20Engine.hpp` mirroring
  `XauusdFvgEngine.hpp`, build the verifier, plan shadow deployment.
- **PARTIAL PASS** (e.g. PF passes but trades count low) → re-spec with looser
  cluster rules or shorter timeframe and re-run.
- **FAIL** → kill the idea, write a brief postmortem next to the spec doc, move on.

## A few things to expect / not be surprised by

- **First run might emit zero trades.** EMA trend filter on a regime with no
  clear trend can shut everything down. Try `--min-nr 2 --trend-slope-lookback 1`
  to loosen things up and confirm the engine fires at all.
- **Costs are conservative.** `--spread-pips 0.5 --slippage-pips 0.3` is
  typical for retail-level XAUUSD; if your actual broker is tighter, lower
  these and re-run.
- **Walk-forward may report fewer than 7 windows.** Depends on how much
  history is in your CSV. The script auto-builds as many as fit.
- **No FVG-correlation check is automated yet.** Spec §10 lists it as a gate
  but it requires running both engines side-by-side on the same data and
  computing correlation of trade entries. Manual step for now — only worth
  doing if everything else passes.

## Dependencies

Pure Python 3.7+. Standard library only (no pandas, no numpy). Should run on
any Mac out of the box.

```bash
python3 --version    # 3.7 or later
```

## Cleanup

Both scripts write CSVs to wherever you run them. Nothing global, nothing
written to your home dir, no env vars touched. Safe to `rm` everything when
done.

## Important: don't commit results to GitHub yet

The `wf_nr2_20_results.csv` can have thousands of rows and isn't useful in
version control. Either keep it local or `.gitignore` it. The five .py + two
.md files are what you'd commit if you decide the ideas are worth tracking.

---

# Part 2 — VWAP comparison suite

The original NR2-20 work above was extended to add VWAP in two ways. You can
run them in any order; results write to separate CSVs.

## VWAP filter inside NR2-20

`nr2_20_backtest.py` now accepts `--trend-filter {ema,vwap,both}`:

```bash
python3 nr2_20_backtest.py --bars XAUUSD_15min.csv \
    --out trades_emafilter.csv --trend-filter ema       # original behavior
python3 nr2_20_backtest.py --bars XAUUSD_15min.csv \
    --out trades_vwapfilter.csv --trend-filter vwap     # VWAP-only filter
python3 nr2_20_backtest.py --bars XAUUSD_15min.csv \
    --out trades_bothfilter.csv --trend-filter both     # require both to agree
```

`vwap` mode requires `close` on the correct side of VWAP **AND** VWAP slope to
agree. `both` mode requires the EMA filter and the VWAP filter to give the same
direction. `both` will produce many fewer trades but should be the highest-
conviction subset.

`--vwap-reset {daily,session,never}` controls the cumulative anchor (daily =
00:00 UTC reset; session = Asia/London/NY boundaries; never = anchor from
first bar onward).

## Standalone VWAP-continuation strategy

`vwap_continuation_backtest.py` is a separate engine implementing the
trend → cross → pullback → continuation flow from the video you summarized.
Sanity-test it first:

```bash
python3 vwap_continuation_backtest.py --self-test
```

Then point at real data:

```bash
python3 vwap_continuation_backtest.py --bars XAUUSD_15min.csv \
    --out vwapc_trades.csv
```

Key params (all overridable via flags):
- `--htf-minutes 60` — higher-timeframe period for trend (60 = 1H, 240 = 4H)
- `--htf-ema-period 50`
- `--pullback-proximity-pips 8` — how close to VWAP counts as a "retest"
- `--pullback-max-age-bars 30` — give up on the setup after this many bars
- `--continuation-lookback 3` — close must beat last 3 bars' high/low
- `--swing-lookback 5` — bars used for SL swing extreme
- `--rr 1.0` — risk/reward (use `1.5` to test asymmetric)

Trades CSV uses the same schema as NR2-20 so `wf_compare.py` can ingest both.

## Head-to-head walk-forward (the actual comparison)

```bash
python3 wf_compare.py --bars XAUUSD_15min.csv --samples 100
```

Runs both strategies through the same rolling 6m-train / 3m-test windows.
For each window: picks the best param set per strategy on train, evaluates on
test, then computes daily-bucket entry correlation between the two strategies'
test-segment trades.

Final block looks like:

```
==========================================================================
VERDICT
==========================================================================
  NR2-20:           PASS gate
  VWAP-continuation: FAIL gate
  Diversification:  N/A (only one strategy traded)

  -> Only NR2-20 viable. Proceed with NR2-20 C++ port.
```

Or:

```
  NR2-20:           PASS gate
  VWAP-continuation: PASS gate
  Diversification:  OK (independent)

  -> Both strategies viable AND complementary. Strong case for porting both.
```

Outputs three CSVs: `wf_compare_nr2_20.csv`, `wf_compare_vwapc.csv`,
`wf_compare_correlation.csv`. The correlation file shows per-window cross-
strategy daily-entry correlation — useful for spotting regimes where the two
strategies happen to align.

For a fast smoke test (smaller grids, fewer windows):

```bash
python3 wf_compare.py --bars XAUUSD_15min.csv --samples 30 --max-windows 3
```

## Interpreting "diversification"

- Mean cross-strategy correlation **< 0.4** → strategies fire on different
  setups; running both adds independent edge. Adopt both.
- Mean cross-strategy correlation **>= 0.4** → strategies double up on the
  same days. Pick whichever has higher Sharpe/PF and drop the other; you'd
  just be doubling position size with extra implementation cost.

## Things that may go wrong on first run

- **VWAP-continuation produces 0 trades.** HTF EMA needs enough warmup
  (default 50 HTF bars × 60 minutes = 50 hours of data) before it returns a
  trend. With short test windows you may starve it. Try
  `--htf-ema-period 20` or `--htf-minutes 60` (instead of 240).
- **`--trend-filter both` gives near-zero trades on NR2-20.** That's expected —
  requiring both EMA and VWAP to agree is a tight filter. Use `vwap` alone
  first to confirm VWAP works at all.
- **Correlation reported as 0.0.** Means one or both strategies emitted no
  trades in the test segment. Look at the per-window summary on stderr to
  see which strategy starved.

