#!/usr/bin/env python3
"""
wf_nr2_20.py
============

Walk-forward driver for the NR2-NR20 + Inside Bar strategy.

This is the actual viability gate. It:
  1. Slices the master XAUUSD bar history into rolling 6m-train / 3m-test windows
     (per HANDOFF_NR2_20_BACKTEST.md §9).
  2. For each window, runs a random sample of N parameter combinations on the
     train segment.
  3. Picks the top-10 param sets by combined score (0.6·Sharpe + 0.4·norm_PF),
     gated on min_trades >= 30 and max_DD_R <= 15.
  4. Applies those top-10 to the held-out test segment.
  5. Aggregates test-segment metrics across all windows.
  6. Compares aggregated metrics against the §10 acceptance gate and prints
     PASS / FAIL per criterion.

Usage
-----
    python wf_nr2_20.py --bars XAUUSD_15min.csv --samples 200

    # Smaller / faster check:
    python wf_nr2_20.py --bars XAUUSD_15min.csv --samples 50 --max-windows 3

    # Custom window scheme (months):
    python wf_nr2_20.py --bars XAUUSD_15min.csv --train-months 12 --test-months 3

Output
------
  - Per-window summary printed to stdout.
  - Aggregate summary at the end.
  - `wf_nr2_20_results.csv`: every train+test run, full param set + metrics.
  - `wf_nr2_20_top.csv`: top-10 per window (test-segment metrics).

Author: Session 2026-05-03
"""

import argparse
import csv
import itertools
import math
import random
import sys
from dataclasses import dataclass, asdict, replace
from datetime import datetime, timedelta, timezone
from typing import List, Tuple, Dict, Optional

# Import from the prototype (must be in same dir)
try:
    from nr2_20_backtest import (
        Bar, Params, Nr2_20Engine, load_bars_csv, summarize,
    )
except ImportError as e:
    print("ERROR: could not import nr2_20_backtest. Place wf_nr2_20.py "
          "in the same directory as nr2_20_backtest.py.", file=sys.stderr)
    raise


# ---------------------------------------------------------------------------
# Param search space (matches HANDOFF spec §8 sweep ranges)
# ---------------------------------------------------------------------------

PARAM_GRID: Dict[str, list] = {
    "min_nr": [2, 4, 7],
    "min_qcb": [2, 3],
    "cluster_lookback": [4, 6, 10],
    "box_expiry": [5, 8, 12],
    "trend_ma_period": [20, 50, 100],
    "trend_slope_lookback": [1, 3, 5],
    "rr": [1.0, 1.5, 2.0],
    "time_stop_bars": [32, 64, 128],
    "same_bar_resolution": ["sl_first", "reject"],
}


def all_combos() -> List[dict]:
    keys = list(PARAM_GRID.keys())
    out = []
    for vals in itertools.product(*[PARAM_GRID[k] for k in keys]):
        out.append(dict(zip(keys, vals)))
    return out


# ---------------------------------------------------------------------------
# Window slicing
# ---------------------------------------------------------------------------

@dataclass
class Window:
    name: str
    train_start: datetime
    train_end: datetime          # exclusive
    test_start: datetime
    test_end: datetime           # exclusive


def add_months(dt: datetime, months: int) -> datetime:
    """Add months to a UTC datetime (clipped to month-end)."""
    y = dt.year + (dt.month - 1 + months) // 12
    m = (dt.month - 1 + months) % 12 + 1
    # Clip day to month length (handles Jan 31 + 1mo → Feb 28)
    import calendar
    last = calendar.monthrange(y, m)[1]
    d = min(dt.day, last)
    return dt.replace(year=y, month=m, day=d)


def make_windows(bars: List[Bar], train_months: int, test_months: int,
                 step_months: int, max_windows: Optional[int]) -> List[Window]:
    """Build rolling windows starting from the first bar's month, stepping by
    step_months until test segment runs past the last bar."""
    if not bars:
        return []
    first = bars[0].ts
    last = bars[-1].ts
    start = first.replace(day=1, hour=0, minute=0, second=0, microsecond=0)
    windows: List[Window] = []
    i = 0
    while True:
        train_start = add_months(start, i * step_months)
        train_end = add_months(train_start, train_months)
        test_start = train_end
        test_end = add_months(test_start, test_months)
        if test_end > last:
            break
        windows.append(Window(
            name=f"W{i+1}",
            train_start=train_start,
            train_end=train_end,
            test_start=test_start,
            test_end=test_end,
        ))
        i += 1
        if max_windows is not None and len(windows) >= max_windows:
            break
    return windows


def slice_bars(bars: List[Bar], start: datetime, end: datetime) -> List[Bar]:
    """Return bars where start <= ts < end. Resets seq numbering on the slice
    so engine bars_seen / entry_bar_seq are window-local."""
    out: List[Bar] = []
    for b in bars:
        if b.ts < start:
            continue
        if b.ts >= end:
            break
        out.append(b)
    # Re-seq within window (engine expects monotonic seq starting at 0)
    for j, b in enumerate(out):
        b.seq = j
    return out


# ---------------------------------------------------------------------------
# Run one param set on one bar slice
# ---------------------------------------------------------------------------

def run_combo(bars: List[Bar], combo: dict, base_costs: dict) -> dict:
    p = Params(
        min_nr=combo["min_nr"],
        min_qcb=combo["min_qcb"],
        cluster_lookback=combo["cluster_lookback"],
        box_expiry=combo["box_expiry"],
        trend_ma_period=combo["trend_ma_period"],
        trend_slope_lookback=combo["trend_slope_lookback"],
        rr=combo["rr"],
        time_stop_bars=combo["time_stop_bars"],
        same_bar_resolution=combo["same_bar_resolution"],
        spread_pips=base_costs["spread_pips"],
        slippage_pips=base_costs["slippage_pips"],
        pip_size=base_costs["pip_size"],
    )
    eng = Nr2_20Engine(p)
    for b in bars:
        eng.step(b)
    return summarize(eng.trades, eng)


# ---------------------------------------------------------------------------
# Scoring
# ---------------------------------------------------------------------------

def score(metrics: dict, min_trades: int = 30, max_dd_R: float = 15.0) -> float:
    """Combined score: 0.6 * Sharpe + 0.4 * normalized_PF.
    Returns -inf if metrics fail the gate."""
    if metrics["trades"] < min_trades:
        return float("-inf")
    if metrics["max_dd_R"] > max_dd_R:
        return float("-inf")
    pf = metrics["profit_factor"]
    if pf == float("inf"):
        pf_norm = 1.0
    else:
        # Squash PF into [0, 1] via tanh-like
        pf_norm = math.tanh((pf - 1.0) / 1.0) if pf > 0 else 0.0
    sharpe = metrics["sharpe_trade"]
    sharpe_norm = math.tanh(sharpe / 2.0)
    return 0.6 * sharpe_norm + 0.4 * pf_norm


# ---------------------------------------------------------------------------
# Aggregate across windows
# ---------------------------------------------------------------------------

def aggregate(test_results: List[dict]) -> dict:
    """Equal-weight aggregate of test-segment metrics."""
    if not test_results:
        return {}
    n = len(test_results)
    keys_avg = ["win_rate", "profit_factor", "avg_R", "sharpe_trade", "max_dd_R"]
    keys_sum = ["trades", "wins", "losses", "total_R"]
    out = {}
    for k in keys_avg:
        vals = [r[k] for r in test_results if r[k] != float("inf")]
        out[k] = sum(vals) / len(vals) if vals else 0.0
    for k in keys_sum:
        out[k] = sum(r[k] for r in test_results)
    return out


# ---------------------------------------------------------------------------
# Acceptance gate (spec §10)
# ---------------------------------------------------------------------------

ACCEPTANCE = {
    "profit_factor": ("ge", 1.30),
    "sharpe_trade":  ("ge", 1.00),
    "max_dd_R":      ("le", 15.0),
    "trades":        ("ge", 200),
    "win_rate":      ("ge", 0.38),
}


def evaluate_gate(agg: dict) -> List[Tuple[str, bool, str]]:
    rows = []
    for metric, (op, threshold) in ACCEPTANCE.items():
        val = agg.get(metric, 0)
        if op == "ge":
            ok = val >= threshold
            cmp = ">="
        else:
            ok = val <= threshold
            cmp = "<="
        rows.append((metric, ok,
                     f"{val:.4f} {cmp} {threshold}  ->  {'PASS' if ok else 'FAIL'}"))
    return rows


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bars", required=True, help="Master OHLC CSV.")
    ap.add_argument("--samples", type=int, default=200,
                    help="Random param combos per train window. Use 0 for "
                         "exhaustive grid (slow: 8748 combos).")
    ap.add_argument("--top-k", type=int, default=10,
                    help="Top-K by score on train; applied to test.")
    ap.add_argument("--train-months", type=int, default=6)
    ap.add_argument("--test-months", type=int, default=3)
    ap.add_argument("--step-months", type=int, default=3)
    ap.add_argument("--max-windows", type=int, default=None)
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--spread-pips", type=float, default=0.5)
    ap.add_argument("--slippage-pips", type=float, default=0.3)
    ap.add_argument("--pip-size", type=float, default=0.10)
    ap.add_argument("--results-csv", default="wf_nr2_20_results.csv")
    ap.add_argument("--top-csv", default="wf_nr2_20_top.csv")
    args = ap.parse_args(argv)

    random.seed(args.seed)

    print(f"Loading bars from {args.bars} ...", file=sys.stderr)
    bars = load_bars_csv(args.bars)
    print(f"  {len(bars)} bars  "
          f"{bars[0].ts.isoformat()} → {bars[-1].ts.isoformat()}", file=sys.stderr)

    windows = make_windows(
        bars, args.train_months, args.test_months, args.step_months,
        args.max_windows,
    )
    print(f"Built {len(windows)} walk-forward windows", file=sys.stderr)
    if not windows:
        print("ERROR: no windows could be built — not enough history.",
              file=sys.stderr)
        return 2

    # Build combo list
    full_grid = all_combos()
    if args.samples == 0 or args.samples >= len(full_grid):
        combos = full_grid
        print(f"Running exhaustive grid: {len(combos)} combos per window",
              file=sys.stderr)
    else:
        combos = random.sample(full_grid, args.samples)
        print(f"Running random sample: {len(combos)} combos per window",
              file=sys.stderr)

    base_costs = dict(
        spread_pips=args.spread_pips,
        slippage_pips=args.slippage_pips,
        pip_size=args.pip_size,
    )

    # Per-window: run all combos on train, pick top-K, run those on test
    all_results_rows: List[dict] = []     # for results-csv
    top_test_results: List[dict] = []     # for aggregate
    top_csv_rows: List[dict] = []

    for w in windows:
        train_bars = slice_bars(bars, w.train_start, w.train_end)
        test_bars = slice_bars(bars, w.test_start, w.test_end)
        print(f"\n=== {w.name}  train {w.train_start.date()} → {w.train_end.date()} "
              f"({len(train_bars)} bars)  test → {w.test_end.date()} "
              f"({len(test_bars)} bars) ===", file=sys.stderr)

        if not train_bars or not test_bars:
            print(f"  skipping {w.name} — empty slice", file=sys.stderr)
            continue

        train_scored: List[Tuple[float, dict, dict]] = []  # (score, combo, metrics)
        for c in combos:
            m = run_combo(train_bars, c, base_costs)
            s = score(m)
            train_scored.append((s, c, m))
            row = {"window": w.name, "phase": "train"}
            row.update(c)
            row.update(m)
            row["score"] = s
            all_results_rows.append(row)

        train_scored.sort(key=lambda x: x[0], reverse=True)
        top = train_scored[:args.top_k]

        # Run top-K on test (track per-window separately so summary is correct
        # even when some top combos fail the train gate)
        win_test_results: List[dict] = []
        for s, c, m in top:
            if s == float("-inf"):
                continue
            tm = run_combo(test_bars, c, base_costs)
            win_test_results.append(tm)
            top_test_results.append(tm)
            row = {"window": w.name, "phase": "test"}
            row.update(c)
            row.update(tm)
            row["train_score"] = s
            top_csv_rows.append(row)
            all_results_rows.append({**{"window": w.name, "phase": "test"},
                                     **c, **tm, "train_score": s})

        # Per-window summary
        if win_test_results:
            agg_w = aggregate(win_test_results)
            print(f"  top-{len(win_test_results)} test agg:  "
                  f"PF={agg_w['profit_factor']:.2f}  "
                  f"Sharpe={agg_w['sharpe_trade']:.2f}  trades={agg_w['trades']}  "
                  f"win%={agg_w['win_rate']*100:.1f}", file=sys.stderr)
        else:
            print(f"  no combos passed train gate — skipping test segment",
                  file=sys.stderr)

    # Write per-run CSV
    if all_results_rows:
        all_keys = sorted({k for r in all_results_rows for k in r.keys()})
        with open(args.results_csv, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=all_keys)
            w.writeheader()
            for r in all_results_rows:
                w.writerow(r)
        print(f"\nWrote per-run results: {args.results_csv}", file=sys.stderr)

    if top_csv_rows:
        keys = sorted({k for r in top_csv_rows for k in r.keys()})
        with open(args.top_csv, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=keys)
            w.writeheader()
            for r in top_csv_rows:
                w.writerow(r)
        print(f"Wrote top-K test runs: {args.top_csv}", file=sys.stderr)

    # Aggregate
    print()
    print("=" * 70)
    print("AGGREGATE — top-K test-segment metrics across all windows")
    print("=" * 70)
    agg = aggregate(top_test_results)
    if not agg:
        print("No test results to aggregate (everything failed train gate).")
        return 1
    for k, v in agg.items():
        if isinstance(v, float):
            print(f"  {k:25s} {v:.4f}")
        else:
            print(f"  {k:25s} {v}")

    # Acceptance gate
    print()
    print("=" * 70)
    print("ACCEPTANCE GATE (HANDOFF_NR2_20_BACKTEST.md §10)")
    print("=" * 70)
    rows = evaluate_gate(agg)
    all_pass = True
    for metric, ok, line in rows:
        print(f"  {metric:20s} {line}")
        if not ok:
            all_pass = False

    print()
    if all_pass:
        print("RESULT: PASS — proceed to C++ port (XauusdNr2_20Engine.hpp)")
        return 0
    else:
        print("RESULT: FAIL — do NOT proceed to C++. Investigate failed metrics, "
              "consider re-spec'ing box construction or shelving the idea.")
        return 1


if __name__ == "__main__":
    sys.exit(main())
