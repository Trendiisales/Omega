#!/usr/bin/env python3
"""
analyze_rt.py
=============

Follow-through rate (R(t)) regime analysis for Omega walkforward trades.

Reads the per-trade OOS CSV files emitted by the modified walkforward_b
and walkforward_b_long binaries and computes:

  R(t) = (count of TP exits) / (count of all completed trades)

binned by entry-month, per (symbol, direction-mode, trade-direction).

The hypothesis being tested is that the H4 Donchian breakout strategy
worked across all 5 symbols in window W1 (the "27/27" period) but
suffered a regime change somewhere in 2025 where breakouts stopped
following through to TP. If R(t) shows a step-change down at a
specific calendar boundary that aligns across symbols, that boundary
is the regime break we need to gate on.

Inputs (any subset present is fine):
  walkforward_b_<SYM>_trades.csv       (base / symmetric harness)
  walkforward_b_long_<SYM>_trades.csv  (long-only harness)

  Where <SYM> in {XAUUSD, NAS100, US500, GER40, EURUSD}.

Outputs:
  rt_monthly.csv          # raw monthly R(t) table (long form)
  rt_summary.txt          # human-readable per-symbol/direction summary
  rt_pivot_base.csv       # pivot: months x symbols, base mode
  rt_pivot_long.csv       # pivot: months x symbols, long mode
  rt_step_change.csv      # detected step-change month per stream

Usage:
  cd ~/Omega/backtest
  python3 analyze_rt.py

Will scan the current directory for matching trades.csv files and
process all that exist. Min 5 trades per month required to compute
R(t) for that bin (else marked NaN).

NO non-stdlib dependencies used — pure Python 3, csv/datetime/math/os.
Runs in seconds even on the full multi-symbol set.
"""

import csv
import datetime
import math
import os
import re
import sys


SYMBOLS = ["XAUUSD", "NAS100", "US500", "GER40", "EURUSD"]
MODES = [
    ("base", "walkforward_b_{sym}_trades.csv"),
    ("long", "walkforward_b_long_{sym}_trades.csv"),
]
MIN_TRADES_PER_MONTH = 5


def load_trades(path):
    """Read a trades.csv. Returns list of dicts."""
    out = []
    with open(path, "r", newline="") as f:
        rdr = csv.DictReader(f)
        for row in rdr:
            out.append(row)
    return out


def entry_month_key(ts_str):
    """'YYYY-MM-DD HH:MM:SS' -> 'YYYY-MM'."""
    # Defensive: handle trailing whitespace or alternative separators.
    ts_str = ts_str.strip()
    # Allow "YYYY-MM-DDTHH:MM:SS" or with space.
    m = re.match(r"^(\d{4})-(\d{2})-(\d{2})", ts_str)
    if not m:
        return None
    return f"{m.group(1)}-{m.group(2)}"


def compute_rt(trades, group_keys):
    """
    Group trades by group_keys (tuple of dict keys), compute R(t).
    Returns dict[group_tuple] -> {n, n_tp, n_sl, n_other, rt}
    """
    buckets = {}
    for t in trades:
        key = tuple(t.get(k, "") for k in group_keys)
        b = buckets.setdefault(
            key, {"n": 0, "n_tp": 0, "n_sl": 0, "n_other": 0,
                  "pnl_sum": 0.0, "pnl_pos": 0, "pnl_neg": 0}
        )
        b["n"] += 1
        reason = t.get("exit_reason", "").strip()
        if reason == "TP":
            b["n_tp"] += 1
        elif reason == "SL":
            b["n_sl"] += 1
        else:
            b["n_other"] += 1
        try:
            pnl = float(t.get("pnl", "0") or 0.0)
        except ValueError:
            pnl = 0.0
        b["pnl_sum"] += pnl
        if pnl > 0:
            b["pnl_pos"] += 1
        elif pnl < 0:
            b["pnl_neg"] += 1
    for b in buckets.values():
        b["rt"] = (b["n_tp"] / b["n"]) if b["n"] >= MIN_TRADES_PER_MONTH else None
        b["wr"] = (b["pnl_pos"] / b["n"]) if b["n"] >= MIN_TRADES_PER_MONTH else None
    return buckets


def detect_step_change(monthly_rt):
    """
    Given an ordered list of (month, rt) with rt possibly None,
    find the month boundary where mean(before) - mean(after) is largest.
    Requires at least 3 valid bins on each side.
    Returns (boundary_month, drop_size, mean_before, mean_after) or None.
    """
    valid = [(m, r) for m, r in monthly_rt if r is not None]
    if len(valid) < 6:
        return None
    months = [m for m, r in valid]
    rts = [r for m, r in valid]
    best = None
    for i in range(3, len(rts) - 2):
        before = rts[:i]
        after = rts[i:]
        mb = sum(before) / len(before)
        ma = sum(after) / len(after)
        drop = mb - ma
        if best is None or drop > best[1]:
            best = (months[i], drop, mb, ma)
    return best


def main():
    rows = []  # long-form rows for rt_monthly.csv
    summary_lines = []
    step_change_rows = []

    summary_lines.append("=" * 72)
    summary_lines.append("R(t) FOLLOW-THROUGH RATE ANALYSIS")
    summary_lines.append("=" * 72)
    summary_lines.append("")
    summary_lines.append(f"Min trades per monthly bin: {MIN_TRADES_PER_MONTH}")
    summary_lines.append("Smaller bins are reported but R(t) shown as NaN.")
    summary_lines.append("")

    found_files = []
    missing_files = []

    for mode, pattern in MODES:
        for sym in SYMBOLS:
            path = pattern.format(sym=sym)
            if not os.path.exists(path):
                missing_files.append(path)
                continue
            found_files.append((mode, sym, path))

    if not found_files:
        print("ERROR: no trades.csv files found in current directory.",
              file=sys.stderr)
        print("Expected files like: walkforward_b_XAUUSD_trades.csv",
              file=sys.stderr)
        sys.exit(1)

    summary_lines.append(f"Found {len(found_files)} trades CSVs:")
    for m, s, p in found_files:
        summary_lines.append(f"  [{m:4s}] {s}: {p}")
    if missing_files:
        summary_lines.append("")
        summary_lines.append(f"Missing ({len(missing_files)}):")
        for p in missing_files:
            summary_lines.append(f"  - {p}")
    summary_lines.append("")

    # Per-stream monthly R(t)
    for mode, sym, path in found_files:
        try:
            trades = load_trades(path)
        except Exception as e:
            summary_lines.append(f"!!! Failed to read {path}: {e}")
            continue

        if not trades:
            summary_lines.append(f"[{mode}/{sym}] empty file, skipping")
            continue

        # Augment each trade with month_key
        for t in trades:
            t["entry_month"] = entry_month_key(t.get("entry_ts", "")) or ""
            t["mode"] = mode
            t["sym"] = sym

        # By (month, direction)
        by_month_dir = compute_rt(trades, ("entry_month", "direction"))
        # By month only
        by_month = compute_rt(trades, ("entry_month",))

        summary_lines.append("-" * 72)
        summary_lines.append(f"[{mode.upper():4s}] {sym}  ({len(trades)} trades, "
                             f"{path})")
        summary_lines.append("-" * 72)

        # Print monthly table
        all_months = sorted(by_month.keys())
        summary_lines.append(
            f"{'month':10s} {'n':>5s} {'n_tp':>5s} {'n_sl':>5s} "
            f"{'R(t)':>8s} {'wr':>6s} {'pnl':>10s}"
        )
        for (m,) in all_months:
            b = by_month[(m,)]
            rt_str = f"{b['rt']:.3f}" if b["rt"] is not None else "  NaN"
            wr_str = f"{b['wr']:.3f}" if b["wr"] is not None else "  NaN"
            summary_lines.append(
                f"{m:10s} {b['n']:>5d} {b['n_tp']:>5d} {b['n_sl']:>5d} "
                f"{rt_str:>8s} {wr_str:>6s} {b['pnl_sum']:>10.2f}"
            )
            rows.append({
                "mode": mode, "symbol": sym, "month": m,
                "direction": "all",
                "n": b["n"], "n_tp": b["n_tp"], "n_sl": b["n_sl"],
                "n_other": b["n_other"],
                "rt": b["rt"], "wr": b["wr"], "pnl_sum": b["pnl_sum"],
            })

        # Step-change detection on the 'all directions' monthly stream
        monthly_pairs = [(m, by_month[(m,)]["rt"]) for (m,) in all_months]
        sc = detect_step_change(monthly_pairs)
        if sc:
            mo, drop, mb, ma = sc
            summary_lines.append(
                f"  STEP CHANGE: at {mo}, R(t) drops {drop:+.3f} "
                f"(before={mb:.3f}, after={ma:.3f})"
            )
            step_change_rows.append({
                "mode": mode, "symbol": sym, "boundary_month": mo,
                "drop": drop, "mean_before": mb, "mean_after": ma,
            })
        else:
            summary_lines.append("  STEP CHANGE: insufficient data")
            step_change_rows.append({
                "mode": mode, "symbol": sym, "boundary_month": "",
                "drop": "", "mean_before": "", "mean_after": "",
            })

        # Long vs short within this stream
        dir_keys = sorted(set((dr,) for (m, dr) in by_month_dir.keys()))
        if len(dir_keys) > 1:
            summary_lines.append("")
            summary_lines.append("  By direction (n_tp / n_sl per month):")
            for (m,) in all_months:
                parts = []
                for (dr,) in dir_keys:
                    b = by_month_dir.get((m, dr))
                    if b is None:
                        parts.append(f"{dr}: -")
                    else:
                        rt_str = f"{b['rt']:.2f}" if b["rt"] is not None else "n/a"
                        parts.append(f"{dr}: {b['n_tp']}/{b['n_sl']} R={rt_str}")
                summary_lines.append(f"    {m}: " + "  ".join(parts))

            for (m, dr), b in by_month_dir.items():
                rows.append({
                    "mode": mode, "symbol": sym, "month": m,
                    "direction": dr,
                    "n": b["n"], "n_tp": b["n_tp"], "n_sl": b["n_sl"],
                    "n_other": b["n_other"],
                    "rt": b["rt"], "wr": b["wr"], "pnl_sum": b["pnl_sum"],
                })

        summary_lines.append("")

    # ---- Write rt_monthly.csv ----
    with open("rt_monthly.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=[
            "mode", "symbol", "month", "direction",
            "n", "n_tp", "n_sl", "n_other",
            "rt", "wr", "pnl_sum"
        ])
        w.writeheader()
        for r in rows:
            r2 = dict(r)
            for k in ("rt", "wr"):
                if r2[k] is None:
                    r2[k] = ""
                else:
                    r2[k] = f"{r2[k]:.4f}"
            r2["pnl_sum"] = f"{r2['pnl_sum']:.2f}"
            w.writerow(r2)
    summary_lines.append("Wrote rt_monthly.csv")

    # ---- Pivot tables: months on rows, symbols on cols, value=R(t) ----
    for mode, _ in MODES:
        all_months = sorted({r["month"] for r in rows
                             if r["mode"] == mode and r["direction"] == "all"})
        all_syms = sorted({r["symbol"] for r in rows
                           if r["mode"] == mode and r["direction"] == "all"})
        pivot_path = f"rt_pivot_{mode}.csv"
        with open(pivot_path, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["month"] + all_syms + ["mean_across_symbols"])
            for m in all_months:
                row_vals = []
                numeric = []
                for s in all_syms:
                    found = [r for r in rows
                             if r["mode"] == mode and r["symbol"] == s
                             and r["month"] == m and r["direction"] == "all"]
                    if found and found[0]["rt"] is not None:
                        v = found[0]["rt"]
                        row_vals.append(f"{v:.3f}")
                        numeric.append(v)
                    else:
                        row_vals.append("")
                mean_str = f"{sum(numeric)/len(numeric):.3f}" if numeric else ""
                w.writerow([m] + row_vals + [mean_str])
        summary_lines.append(f"Wrote {pivot_path}")

    # ---- Write rt_step_change.csv ----
    with open("rt_step_change.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=[
            "mode", "symbol", "boundary_month",
            "drop", "mean_before", "mean_after"
        ])
        w.writeheader()
        for r in step_change_rows:
            r2 = dict(r)
            for k in ("drop", "mean_before", "mean_after"):
                if isinstance(r2[k], float):
                    r2[k] = f"{r2[k]:.4f}"
            w.writerow(r2)
    summary_lines.append("Wrote rt_step_change.csv")

    # ---- Cross-symbol regime alignment summary ----
    summary_lines.append("")
    summary_lines.append("=" * 72)
    summary_lines.append("CROSS-SYMBOL STEP-CHANGE ALIGNMENT")
    summary_lines.append("=" * 72)
    for mode, _ in MODES:
        relevant = [r for r in step_change_rows
                    if r["mode"] == mode and r["boundary_month"]]
        if not relevant:
            continue
        summary_lines.append("")
        summary_lines.append(f"[{mode.upper()}] step-change months by symbol:")
        for r in relevant:
            summary_lines.append(
                f"  {r['symbol']:7s}  break at {r['boundary_month']}  "
                f"R(t) drop {r['drop']:+.3f}  "
                f"({r['mean_before']:.3f} -> {r['mean_after']:.3f})"
            )

    # Write summary file
    with open("rt_summary.txt", "w") as f:
        f.write("\n".join(summary_lines) + "\n")

    # Echo to stdout too
    for line in summary_lines:
        print(line)


if __name__ == "__main__":
    main()
