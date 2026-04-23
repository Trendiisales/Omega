#!/usr/bin/env python3
# =============================================================================
# analyze_cbe_sweep.py -- Analyze CBE REENTER_COMP sweep results
#
# Reads:
#   backtest/results/cbe_sweep/sweep_index.csv          (cell params)
#   backtest/results/cbe_sweep/<cell_id>/train.csv      (trades)
#   backtest/results/cbe_sweep/<cell_id>/test.csv       (trades)
#
# Emits:
#   backtest/results/cbe_sweep/summary.csv              (per-cell stats)
#   stdout                                              (ranked table)
#
# INTERPRETATION GATE:
#   A cell "passes" iff ALL of:
#     - train T >= 30
#     - test  T >= 30
#     - train net PnL > 0
#     - test  net PnL > 0
#     - no full-split max drawdown exceeds $100
#
#   The baseline "disabled" cell is compared against every passing cell.
#   A sweep finding of "change gate" requires at least one passing cell
#   to beat the disabled baseline on BOTH train and test PnL.
# =============================================================================

import argparse
import csv
import os
import sys
from pathlib import Path


def compute_stats(trade_csv):
    """Compute {T, W, WR, net, max_dd, median_hold_s, reenter_frac,
                reenter_pnl, sl_pnl, tp_pnl, timeout_pnl, trail_pnl, be_pnl}
    from a trades CSV."""
    if not trade_csv.exists():
        return None

    rows = []
    with open(trade_csv, newline="") as f:
        reader = csv.DictReader(f)
        for r in reader:
            rows.append(r)

    n = len(rows)
    stats = {
        "T": n,
        "W": 0,
        "WR": 0.0,
        "net": 0.0,
        "max_dd": 0.0,
        "median_hold_s": 0.0,
        "reenter_frac": 0.0,
        "reenter_pnl": 0.0,
        "sl_pnl": 0.0,
        "tp_pnl": 0.0,
        "timeout_pnl": 0.0,
        "trail_pnl": 0.0,
        "be_pnl": 0.0,
    }
    if n == 0:
        return stats

    by_reason = {}
    holds = []
    cum = 0.0
    peak = 0.0
    max_dd = 0.0
    for r in rows:
        pnl = float(r["pnl_usd"])
        stats["net"] += pnl
        if pnl > 0:
            stats["W"] += 1
        reason = r["exit_reason"]
        by_reason.setdefault(reason, {"count": 0, "pnl": 0.0})
        by_reason[reason]["count"] += 1
        by_reason[reason]["pnl"] += pnl

        holds.append(int(r["held_ms"]) / 1000.0)

        cum += pnl
        if cum > peak:
            peak = cum
        dd = peak - cum
        if dd > max_dd:
            max_dd = dd

    stats["WR"] = 100.0 * stats["W"] / n
    stats["max_dd"] = max_dd
    holds.sort()
    stats["median_hold_s"] = holds[n // 2] if n > 0 else 0.0

    stats["reenter_frac"] = 100.0 * by_reason.get("REENTER_COMP", {"count": 0})["count"] / n
    stats["reenter_pnl"]  = by_reason.get("REENTER_COMP", {"pnl": 0.0})["pnl"]
    stats["sl_pnl"]       = by_reason.get("SL_HIT",       {"pnl": 0.0})["pnl"]
    stats["tp_pnl"]       = by_reason.get("TP_HIT",       {"pnl": 0.0})["pnl"]
    stats["timeout_pnl"]  = by_reason.get("TIMEOUT",      {"pnl": 0.0})["pnl"]
    stats["trail_pnl"]    = by_reason.get("TRAIL_HIT",    {"pnl": 0.0})["pnl"]
    stats["be_pnl"]       = by_reason.get("BE_HIT",       {"pnl": 0.0})["pnl"]

    return stats


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--results-dir", default="backtest/results/cbe_sweep",
                    help="Results directory (default: backtest/results/cbe_sweep)")
    ap.add_argument("--min-t", type=int, default=30,
                    help="Minimum trades per split to pass interpretation gate")
    ap.add_argument("--max-dd", type=float, default=100.0,
                    help="Maximum allowed drawdown per split ($)")
    args = ap.parse_args()

    results_dir = Path(args.results_dir)
    index_csv = results_dir / "sweep_index.csv"
    if not index_csv.exists():
        print(f"[ERROR] Sweep index not found: {index_csv}", file=sys.stderr)
        sys.exit(1)

    # Read cell index
    cells = []
    with open(index_csv, newline="") as f:
        reader = csv.DictReader(f)
        for r in reader:
            cells.append(r)
    print(f"[INFO] Loaded {len(cells)} cells from index")

    # Compute stats for each cell
    summary = []
    for c in cells:
        cell_id = c["cell_id"]
        train_csv = results_dir / cell_id / "train.csv"
        test_csv  = results_dir / cell_id / "test.csv"

        train = compute_stats(train_csv)
        test  = compute_stats(test_csv)

        if train is None or test is None:
            print(f"[WARN] Missing results for cell {cell_id}", file=sys.stderr)
            continue

        passed = (
            train["T"] >= args.min_t and
            test["T"]  >= args.min_t and
            train["net"] > 0 and
            test["net"]  > 0 and
            train["max_dd"] <= args.max_dd and
            test["max_dd"]  <= args.max_dd
        )

        row = {
            "cell_id":   cell_id,
            "mode":      c["mode"],
            "tol":       c["reenter_tol"],
            "be":        c["needs_be"],
            "hold_ms":   c["min_hold_ms"],
            "train_T":   train["T"],
            "train_WR":  train["WR"],
            "train_net": train["net"],
            "train_dd":  train["max_dd"],
            "train_reenter_pct": train["reenter_frac"],
            "test_T":    test["T"],
            "test_WR":   test["WR"],
            "test_net":  test["net"],
            "test_dd":   test["max_dd"],
            "test_reenter_pct": test["reenter_frac"],
            "total_net": train["net"] + test["net"],
            "min_split_net": min(train["net"], test["net"]),
            "pass":      passed,
        }
        summary.append(row)

    # Find baseline (disabled)
    baseline = next((r for r in summary if r["mode"] == "disabled"), None)
    if baseline is None:
        print("[WARN] No disabled baseline found in results")
    else:
        print(f"\n[BASELINE] disabled: "
              f"train T={baseline['train_T']} WR={baseline['train_WR']:.1f}% "
              f"net=${baseline['train_net']:.2f} dd=${baseline['train_dd']:.2f} | "
              f"test T={baseline['test_T']} WR={baseline['test_WR']:.1f}% "
              f"net=${baseline['test_net']:.2f} dd=${baseline['test_dd']:.2f}")

    # Write summary CSV
    summary_csv = results_dir / "summary.csv"
    if summary:
        fieldnames = list(summary[0].keys())
        with open(summary_csv, "w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=fieldnames)
            writer.writeheader()
            for r in summary:
                writer.writerow(r)
        print(f"[OK] Wrote {summary_csv}")

    # ---- Rank: all cells, sorted by min(train_net, test_net) desc ----
    summary_sorted = sorted(summary, key=lambda r: r["min_split_net"], reverse=True)

    print()
    print("=" * 110)
    print("ALL CELLS -- ranked by min(train_net, test_net)")
    print("=" * 110)
    hdr = ("cell_id            mode       tol    be  hold   "
           "train_T train_WR  train_net  train_dd  test_T  test_WR  test_net  test_dd  pass")
    print(hdr)
    print("-" * 110)
    for r in summary_sorted:
        print(f"{r['cell_id']:18s} {r['mode']:9s} "
              f"{r['tol']:>5s}  {r['be']:>2s}  {r['hold_ms']:>5s}   "
              f"{r['train_T']:>6}  {r['train_WR']:>6.1f}%  "
              f"${r['train_net']:>8.2f}  ${r['train_dd']:>7.2f}  "
              f"{r['test_T']:>5}  {r['test_WR']:>6.1f}%  "
              f"${r['test_net']:>7.2f}  ${r['test_dd']:>6.2f}   "
              f"{'Y' if r['pass'] else '-'}")

    # ---- Passing cells only ----
    passing = [r for r in summary_sorted if r["pass"]]
    print()
    print("=" * 110)
    print(f"PASSING CELLS (T>={args.min_t}, both nets>0, dd<=${args.max_dd})  "
          f"-- {len(passing)}/{len(summary)}")
    print("=" * 110)
    if not passing:
        print("NONE. No configuration satisfies the interpretation gate on both splits.")
    else:
        for r in passing:
            print(f"  {r['cell_id']:18s}  "
                  f"train ${r['train_net']:>7.2f} ({r['train_T']}t WR{r['train_WR']:.0f}%)  "
                  f"test ${r['test_net']:>7.2f} ({r['test_T']}t WR{r['test_WR']:.0f}%)  "
                  f"min=${r['min_split_net']:.2f}")

    # ---- Verdict ----
    print()
    print("=" * 110)
    print("VERDICT")
    print("=" * 110)
    if baseline is None:
        print("No baseline to compare against. Inconclusive.")
        return

    bl_min = min(baseline["train_net"], baseline["test_net"])
    better = [r for r in passing if r["min_split_net"] > bl_min]
    if not better:
        print(f"No passing cell beats the disabled baseline (min_net=${bl_min:.2f})")
        print(f"  Recommendation: REVERT REENTER_COMP gate to pre-CBE-4 state")
        print(f"  (equivalent to the 'disabled' cell -- gate effectively off)")
    else:
        print(f"Cells beating disabled baseline on BOTH splits: {len(better)}")
        for r in better[:5]:
            delta_train = r["train_net"] - baseline["train_net"]
            delta_test  = r["test_net"]  - baseline["test_net"]
            print(f"  {r['cell_id']:18s}  "
                  f"train +${delta_train:>6.2f}  test +${delta_test:>6.2f}  "
                  f"min=${r['min_split_net']:.2f}")
        best = better[0]
        print()
        print(f"BEST CELL: {best['cell_id']}")
        print(f"  tol={best['tol']}  needs_be={best['be']}  min_hold_ms={best['hold_ms']}")
        print(f"  train: T={best['train_T']}  WR={best['train_WR']:.1f}%  "
              f"net=${best['train_net']:.2f}  dd=${best['train_dd']:.2f}")
        print(f"  test:  T={best['test_T']}  WR={best['test_WR']:.1f}%  "
              f"net=${best['test_net']:.2f}  dd=${best['test_dd']:.2f}")


if __name__ == "__main__":
    main()
