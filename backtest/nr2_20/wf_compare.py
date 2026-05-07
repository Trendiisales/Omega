#!/usr/bin/env python3
"""
wf_compare.py
=============

Walk-forward + side-by-side comparison of:
  (A) NR2-20 compression-breakout (with EMA, VWAP, or both as trend filter)
  (B) VWAP-continuation strategy

For each rolling train/test window:
  - Picks the best NR2-20 param set on train, evaluates on test.
  - Picks the best VWAP-continuation param set on train, evaluates on test.
  - Records per-strategy test metrics + signal-overlap correlation.

At the end:
  - Aggregate test-segment metrics for each strategy.
  - Acceptance gate (HANDOFF §10) verdict for each.
  - Pearson correlation of trade-entry timestamps (bucketed by day) — proxy for
    whether the two strategies fire together. Low correlation = good (they
    diversify). High correlation = redundant.

Usage
-----
  python3 wf_compare.py --bars XAUUSD_15min.csv --samples 100 --max-windows 4

Output
------
  - Per-strategy per-window summary to stderr.
  - Final comparison block to stdout.
  - wf_compare_nr2_20.csv  : top-K per window, NR2-20
  - wf_compare_vwapc.csv   : top-K per window, VWAP-continuation
  - wf_compare_correlation.csv : per-window correlation breakdown

Author: Session 2026-05-03
"""

import argparse
import csv
import itertools
import math
import random
import sys
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from typing import List, Tuple, Dict, Optional, Callable

# Bring in shared loaders + window slicing
from nr2_20_backtest import Bar, load_bars_csv, Params as NrParams, \
    Nr2_20Engine, summarize as nr_summarize
from vwap_continuation_backtest import (
    VwapcParams, VwapContinuationEngine, summarize as vwapc_summarize,
)
from wf_nr2_20 import (
    Window, make_windows, slice_bars, score, evaluate_gate, ACCEPTANCE,
)


# ---------------------------------------------------------------------------
# Param grids per strategy (subsets of the full grids — keeps comparison run
# tractable; full grids are still in wf_nr2_20.py / vwap_continuation_backtest)
# ---------------------------------------------------------------------------

NR_GRID: Dict[str, list] = {
    "min_nr": [4, 7],
    "min_qcb": [2],
    "cluster_lookback": [4, 6],
    "box_expiry": [5, 8],
    "trend_ma_period": [20, 50],
    "trend_slope_lookback": [3],
    "rr": [1.0, 1.5],
    "time_stop_bars": [32, 64],
    "same_bar_resolution": ["sl_first"],
    "trend_filter": ["ema", "vwap", "both"],   # this is the comparison axis
    "vwap_reset": ["daily"],
    "vwap_slope_lookback": [3],
}

VWAPC_GRID: Dict[str, list] = {
    "htf_minutes": [60, 240],
    "htf_ema_period": [20, 50],
    "htf_slope_lookback": [3],
    "vwap_reset": ["daily"],
    "pullback_proximity_pips": [5.0, 10.0, 20.0],
    "pullback_max_age_bars": [20, 30],
    "continuation_lookback": [3, 5],
    "swing_lookback": [5, 10],
    "swing_buffer_pips": [3.0, 5.0],
    "rr": [1.0, 1.5],
    "time_stop_bars": [64, 128],
    "same_bar_resolution": ["sl_first"],
}


def all_combos(grid: Dict[str, list]) -> List[dict]:
    keys = list(grid.keys())
    return [dict(zip(keys, vals))
            for vals in itertools.product(*[grid[k] for k in keys])]


# ---------------------------------------------------------------------------
# Strategy runners
# ---------------------------------------------------------------------------

def run_nr2_20(bars: List[Bar], combo: dict, base_costs: dict) -> Tuple[dict, list]:
    p = NrParams(
        min_nr=combo["min_nr"],
        min_qcb=combo["min_qcb"],
        cluster_lookback=combo["cluster_lookback"],
        box_expiry=combo["box_expiry"],
        trend_ma_period=combo["trend_ma_period"],
        trend_slope_lookback=combo["trend_slope_lookback"],
        rr=combo["rr"],
        time_stop_bars=combo["time_stop_bars"],
        same_bar_resolution=combo["same_bar_resolution"],
        trend_filter=combo["trend_filter"],
        vwap_reset=combo["vwap_reset"],
        vwap_slope_lookback=combo["vwap_slope_lookback"],
        spread_pips=base_costs["spread_pips"],
        slippage_pips=base_costs["slippage_pips"],
        pip_size=base_costs["pip_size"],
    )
    eng = Nr2_20Engine(p)
    for b in bars:
        eng.step(b)
    return nr_summarize(eng.trades, eng), eng.trades


def run_vwapc(bars: List[Bar], combo: dict, base_costs: dict) -> Tuple[dict, list]:
    p = VwapcParams(
        htf_minutes=combo["htf_minutes"],
        htf_ema_period=combo["htf_ema_period"],
        htf_slope_lookback=combo["htf_slope_lookback"],
        vwap_reset=combo["vwap_reset"],
        pullback_proximity_pips=combo["pullback_proximity_pips"],
        pullback_max_age_bars=combo["pullback_max_age_bars"],
        continuation_lookback=combo["continuation_lookback"],
        swing_lookback=combo["swing_lookback"],
        swing_buffer_pips=combo["swing_buffer_pips"],
        rr=combo["rr"],
        time_stop_bars=combo["time_stop_bars"],
        same_bar_resolution=combo["same_bar_resolution"],
        spread_pips=base_costs["spread_pips"],
        slippage_pips=base_costs["slippage_pips"],
        pip_size=base_costs["pip_size"],
    )
    eng = VwapContinuationEngine(p)
    for b in bars:
        eng.step(b)
    return vwapc_summarize(eng.trades, eng), eng.trades


# ---------------------------------------------------------------------------
# Aggregation + correlation
# ---------------------------------------------------------------------------

def aggregate(test_results: List[dict]) -> dict:
    if not test_results:
        return {}
    keys_avg = ["win_rate", "profit_factor", "avg_R", "sharpe_trade", "max_dd_R"]
    keys_sum = ["trades", "wins", "losses", "total_R"]
    out = {}
    for k in keys_avg:
        vals = [r[k] for r in test_results
                if r.get(k) is not None and r[k] != float("inf")]
        out[k] = sum(vals) / len(vals) if vals else 0.0
    for k in keys_sum:
        out[k] = sum(r.get(k, 0) for r in test_results)
    return out


def daily_buckets(trades: list) -> Dict[str, int]:
    """Count trade entries per UTC day."""
    out: Dict[str, int] = {}
    for t in trades:
        day = t.entry_ts.date().isoformat()
        out[day] = out.get(day, 0) + 1
    return out


def pearson(xs: List[float], ys: List[float]) -> float:
    """Plain Pearson correlation between two equal-length series."""
    n = len(xs)
    if n < 2 or n != len(ys):
        return 0.0
    mean_x = sum(xs) / n
    mean_y = sum(ys) / n
    cov = sum((xs[i] - mean_x) * (ys[i] - mean_y) for i in range(n))
    var_x = sum((x - mean_x) ** 2 for x in xs)
    var_y = sum((y - mean_y) ** 2 for y in ys)
    denom = math.sqrt(var_x * var_y)
    return cov / denom if denom > 0 else 0.0


def correlate_strategies(nr_trades: list, vwapc_trades: list) -> float:
    """Daily-bucket entry-count correlation between the two strategies. The
    closer to 1.0, the more they fire on the same days — i.e. redundant."""
    nr_days = daily_buckets(nr_trades)
    vw_days = daily_buckets(vwapc_trades)
    all_days = sorted(set(nr_days) | set(vw_days))
    if not all_days:
        return 0.0
    xs = [nr_days.get(d, 0) for d in all_days]
    ys = [vw_days.get(d, 0) for d in all_days]
    return pearson(xs, ys)


# ---------------------------------------------------------------------------
# Per-window driver
# ---------------------------------------------------------------------------

def best_combo_on_train(combos: list, bars: list, base_costs: dict,
                        runner: Callable) -> Optional[Tuple[dict, dict]]:
    """Run all combos on train bars, return (best_combo, best_metrics) or
    None if none cleared the score gate."""
    best_score = float("-inf")
    best = None
    for c in combos:
        m, _ = runner(bars, c, base_costs)
        s = score(m)
        if s > best_score:
            best_score = s
            best = (c, m)
    if best_score == float("-inf"):
        return None
    return best


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bars", required=True)
    ap.add_argument("--samples", type=int, default=100,
                    help="Random samples per strategy per window (0 = exhaustive).")
    ap.add_argument("--train-months", type=int, default=6)
    ap.add_argument("--test-months", type=int, default=3)
    ap.add_argument("--step-months", type=int, default=3)
    ap.add_argument("--max-windows", type=int, default=None)
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--spread-pips", type=float, default=0.5)
    ap.add_argument("--slippage-pips", type=float, default=0.3)
    ap.add_argument("--pip-size", type=float, default=0.10)
    args = ap.parse_args(argv)

    random.seed(args.seed)

    print(f"Loading bars from {args.bars} ...", file=sys.stderr)
    bars = load_bars_csv(args.bars)
    print(f"  {len(bars)} bars  "
          f"{bars[0].ts.isoformat()} → {bars[-1].ts.isoformat()}", file=sys.stderr)

    windows = make_windows(bars, args.train_months, args.test_months,
                           args.step_months, args.max_windows)
    print(f"Built {len(windows)} walk-forward windows", file=sys.stderr)
    if not windows:
        print("ERROR: no windows.", file=sys.stderr)
        return 2

    # Build combo lists
    nr_full = all_combos(NR_GRID)
    vw_full = all_combos(VWAPC_GRID)
    if args.samples == 0:
        nr_combos, vw_combos = nr_full, vw_full
    else:
        nr_combos = (random.sample(nr_full, args.samples)
                     if args.samples < len(nr_full) else nr_full)
        vw_combos = (random.sample(vw_full, args.samples)
                     if args.samples < len(vw_full) else vw_full)
    print(f"NR2-20 combos:    {len(nr_combos)} (full {len(nr_full)})",
          file=sys.stderr)
    print(f"VWAPC combos:     {len(vw_combos)} (full {len(vw_full)})",
          file=sys.stderr)

    base_costs = dict(spread_pips=args.spread_pips,
                      slippage_pips=args.slippage_pips,
                      pip_size=args.pip_size)

    nr_test_results: List[dict] = []
    vw_test_results: List[dict] = []
    nr_top_rows: List[dict] = []
    vw_top_rows: List[dict] = []
    correlation_rows: List[dict] = []

    for w in windows:
        train_bars = slice_bars(bars, w.train_start, w.train_end)
        test_bars = slice_bars(bars, w.test_start, w.test_end)
        print(f"\n=== {w.name}  train {w.train_start.date()} → {w.train_end.date()} "
              f"({len(train_bars)} bars)  test → {w.test_end.date()} "
              f"({len(test_bars)} bars) ===", file=sys.stderr)
        if not train_bars or not test_bars:
            continue

        # NR2-20: train, then test
        nr_best = best_combo_on_train(nr_combos, train_bars, base_costs, run_nr2_20)
        if nr_best is None:
            print(f"  NR2-20: no combo passed train gate", file=sys.stderr)
            nr_trades_test = []
        else:
            c, _ = nr_best
            tm, nr_trades_test = run_nr2_20(test_bars, c, base_costs)
            nr_test_results.append(tm)
            row = {"window": w.name}; row.update(c); row.update(tm)
            nr_top_rows.append(row)
            print(f"  NR2-20 ({c['trend_filter']}): "
                  f"PF={tm['profit_factor']:.2f}  "
                  f"Sharpe={tm['sharpe_trade']:.2f}  "
                  f"trades={tm['trades']}  win%={tm['win_rate']*100:.1f}",
                  file=sys.stderr)

        # VWAP-continuation: train, then test
        vw_best = best_combo_on_train(vw_combos, train_bars, base_costs, run_vwapc)
        if vw_best is None:
            print(f"  VWAPC: no combo passed train gate", file=sys.stderr)
            vw_trades_test = []
        else:
            c, _ = vw_best
            tm, vw_trades_test = run_vwapc(test_bars, c, base_costs)
            vw_test_results.append(tm)
            row = {"window": w.name}; row.update(c); row.update(tm)
            vw_top_rows.append(row)
            print(f"  VWAPC: PF={tm['profit_factor']:.2f}  "
                  f"Sharpe={tm['sharpe_trade']:.2f}  "
                  f"trades={tm['trades']}  win%={tm['win_rate']*100:.1f}",
                  file=sys.stderr)

        # Correlation on test segment
        if nr_trades_test and vw_trades_test:
            corr = correlate_strategies(nr_trades_test, vw_trades_test)
        else:
            corr = 0.0
        correlation_rows.append({
            "window": w.name,
            "nr_trades": len(nr_trades_test),
            "vwapc_trades": len(vw_trades_test),
            "daily_entry_correlation": corr,
        })
        print(f"  Cross-strategy daily-entry correlation: {corr:+.3f}",
              file=sys.stderr)

    # Write per-window CSVs
    if nr_top_rows:
        keys = sorted({k for r in nr_top_rows for k in r.keys()})
        with open("wf_compare_nr2_20.csv", "w", newline="") as f:
            wr = csv.DictWriter(f, fieldnames=keys)
            wr.writeheader()
            for r in nr_top_rows:
                wr.writerow(r)
    if vw_top_rows:
        keys = sorted({k for r in vw_top_rows for k in r.keys()})
        with open("wf_compare_vwapc.csv", "w", newline="") as f:
            wr = csv.DictWriter(f, fieldnames=keys)
            wr.writeheader()
            for r in vw_top_rows:
                wr.writerow(r)
    if correlation_rows:
        keys = list(correlation_rows[0].keys())
        with open("wf_compare_correlation.csv", "w", newline="") as f:
            wr = csv.DictWriter(f, fieldnames=keys)
            wr.writeheader()
            for r in correlation_rows:
                wr.writerow(r)

    # Aggregate + gate verdicts
    print()
    print("=" * 76)
    print("AGGREGATE — out-of-sample test metrics across all windows")
    print("=" * 76)

    nr_agg = aggregate(nr_test_results)
    vw_agg = aggregate(vw_test_results)

    print()
    print(f"  {'metric':25s}  {'NR2-20':>14s}  {'VWAPC':>14s}")
    print(f"  {'-'*25}  {'-'*14}  {'-'*14}")
    if not nr_agg and not vw_agg:
        print("  (no test results for either strategy)")
        return 1
    for k in ["trades", "win_rate", "profit_factor", "sharpe_trade",
              "max_dd_R", "total_R", "avg_R"]:
        nrv = nr_agg.get(k, 0)
        vwv = vw_agg.get(k, 0)
        nrs = f"{nrv:.4f}" if isinstance(nrv, float) else str(nrv)
        vws = f"{vwv:.4f}" if isinstance(vwv, float) else str(vwv)
        print(f"  {k:25s}  {nrs:>14s}  {vws:>14s}")

    print()
    print("=" * 76)
    print("ACCEPTANCE GATES (per HANDOFF_NR2_20_BACKTEST.md §10)")
    print("=" * 76)
    print()
    print(f"  {'metric':20s}  {'NR2-20 verdict':>30s}  {'VWAPC verdict':>30s}")
    print(f"  {'-'*20}  {'-'*30}  {'-'*30}")
    nr_pass_all = True
    vw_pass_all = True
    for metric, (op, threshold) in ACCEPTANCE.items():
        nrv = nr_agg.get(metric, 0) if nr_agg else 0
        vwv = vw_agg.get(metric, 0) if vw_agg else 0
        cmp = ">=" if op == "ge" else "<="
        if op == "ge":
            nr_ok = nrv >= threshold
            vw_ok = vwv >= threshold
        else:
            nr_ok = nrv <= threshold
            vw_ok = vwv <= threshold
        if not nr_ok:
            nr_pass_all = False
        if not vw_ok:
            vw_pass_all = False
        nr_str = f"{nrv:.4f} {cmp} {threshold}  {'PASS' if nr_ok else 'FAIL'}"
        vw_str = f"{vwv:.4f} {cmp} {threshold}  {'PASS' if vw_ok else 'FAIL'}"
        print(f"  {metric:20s}  {nr_str:>30s}  {vw_str:>30s}")

    # Cross-strategy correlation (mean across windows)
    if correlation_rows:
        valid = [r["daily_entry_correlation"] for r in correlation_rows
                 if r["nr_trades"] > 0 and r["vwapc_trades"] > 0]
        mean_corr = sum(valid) / len(valid) if valid else 0.0
    else:
        mean_corr = 0.0

    print()
    print(f"  Cross-strategy mean daily-entry correlation: {mean_corr:+.4f}")
    print(f"    (>= 0.40 = redundant; < 0.40 = independent edge)")
    diversifies = abs(mean_corr) < 0.40

    print()
    print("=" * 76)
    print("VERDICT")
    print("=" * 76)
    print(f"  NR2-20:           {'PASS gate' if nr_pass_all else 'FAIL gate'}")
    print(f"  VWAP-continuation: {'PASS gate' if vw_pass_all else 'FAIL gate'}")
    print(f"  Diversification:  {'OK (independent)' if diversifies else 'POOR (correlated)'}")
    print()
    if nr_pass_all and vw_pass_all and diversifies:
        print("  -> Both strategies viable AND complementary. Strong case for porting both.")
        rc = 0
    elif nr_pass_all and vw_pass_all and not diversifies:
        print("  -> Both viable but redundant. Pick the one with better Sharpe/PF, drop the other.")
        rc = 0
    elif nr_pass_all and not vw_pass_all:
        print("  -> Only NR2-20 viable. Proceed with NR2-20 C++ port.")
        rc = 0
    elif vw_pass_all and not nr_pass_all:
        print("  -> Only VWAP-continuation viable. Re-spec NR2-20 or shelve, port VWAPC.")
        rc = 0
    else:
        print("  -> Neither strategy clears the gate. Investigate or shelve both.")
        rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
