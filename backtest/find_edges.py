#!/usr/bin/env python3
"""
find_edges.py — Edge discovery on OOS trades.

Reads walkforward_b[_long]_<SYM>_trades.csv files and answers three questions:

  Q1. Direction asymmetry: is one direction profitable across all windows?
  Q2. ATR-at-entry buckets: does volatility-at-entry predict outcome?
  Q3. bars_held cohorts: do fast vs slow trades have different EV?

Pure stdlib. Run from ~/Omega/backtest after analyze_rt.py has been run.

Outputs:
  edges_summary.txt   — human-readable findings
"""

import csv
import os
import glob
from collections import defaultdict
from datetime import datetime


def load(path):
    rows = []
    if not os.path.exists(path):
        return rows
    with open(path) as f:
        for r in csv.DictReader(f):
            try:
                rows.append({
                    "window": r["window"],
                    "direction": r["direction"],
                    "entry_ts": datetime.strptime(r["entry_ts"], "%Y-%m-%d %H:%M:%S"),
                    "pnl": float(r["pnl"]),
                    "exit_reason": r["exit_reason"],
                    "bars_held": int(r["bars_held"]),
                    "atr": float(r.get("atr_at_entry", "0") or 0),
                })
            except (ValueError, KeyError):
                continue
    return rows


def stats(trades):
    if not trades:
        return None
    n = len(trades)
    pnls = [t["pnl"] for t in trades]
    wins = [p for p in pnls if p > 0]
    losses = [p for p in pnls if p <= 0]
    gp = sum(wins)
    gl = sum(losses)
    pf = (gp / -gl) if gl < 0 else (1e9 if gp > 0 else 0.0)
    wr = len(wins) / n
    return {
        "n": n,
        "pnl": sum(pnls),
        "pf": pf,
        "wr": wr,
        "avg_win": (gp / len(wins)) if wins else 0.0,
        "avg_loss": (gl / len(losses)) if losses else 0.0,
    }


def fmt(s):
    if s is None:
        return "n=0"
    return (f"n={s['n']:3d} pnl={s['pnl']:+9.1f} "
            f"pf={s['pf']:5.2f} wr={s['wr']:.2f} "
            f"avgW={s['avg_win']:+6.1f} avgL={s['avg_loss']:+6.1f}")


def percentile_buckets(values, n_buckets=3):
    """Return (low, mid_lo, mid_hi, high) edges for tertile (n_buckets=3) or
    quartile (4) buckets. Edges are inclusive-low, exclusive-high except last."""
    s = sorted(values)
    n = len(s)
    if n < n_buckets * 2:
        return None
    edges = [s[int(n * i / n_buckets)] for i in range(n_buckets + 1)]
    edges[0] = s[0]
    edges[-1] = s[-1] + 1e-12
    return edges


def main():
    out = []
    out.append("Edge discovery on walkforward OOS trades")
    out.append("=" * 70)
    out.append("")

    # Group all trades by (symbol, mode)
    file_pattern = sorted(glob.glob("walkforward_b*_trades.csv"))
    if not file_pattern:
        out.append("ERROR: no walkforward_b*_trades.csv files found in cwd")
        with open("edges_summary.txt", "w") as f:
            f.write("\n".join(out))
        print("\n".join(out))
        return

    by_symmode = {}
    for path in file_pattern:
        # parse filename: walkforward_b_<SYM>_trades.csv or walkforward_b_long_<SYM>_trades.csv
        base = os.path.basename(path).replace("_trades.csv", "")
        if base.startswith("walkforward_b_long_"):
            sym = base[len("walkforward_b_long_"):]
            mode = "long"
        elif base.startswith("walkforward_b_"):
            sym = base[len("walkforward_b_"):]
            mode = "base"
        else:
            continue
        by_symmode[(sym, mode)] = load(path)

    # ========== Q1: Direction asymmetry across windows ==========
    out.append("")
    out.append("=" * 70)
    out.append("Q1. DIRECTION ASYMMETRY (base mode only)")
    out.append("=" * 70)
    out.append("Per-symbol per-window per-direction. If long is profitable in all")
    out.append("windows or short is profitable in all windows, that's a stable edge.")
    out.append("")

    for sym in ["XAUUSD", "NAS100", "US500", "GER40", "EURUSD"]:
        trades = by_symmode.get((sym, "base"), [])
        if not trades:
            continue
        out.append(f"--- {sym} base ---")
        # For each window, split by direction
        for win in ["W2", "W3", "W4"]:
            wt = [t for t in trades if t["window"] == win]
            longs = [t for t in wt if t["direction"] == "long"]
            shorts = [t for t in wt if t["direction"] == "short"]
            sL = stats(longs)
            sS = stats(shorts)
            out.append(f"  {win} long:  {fmt(sL)}")
            out.append(f"  {win} short: {fmt(sS)}")
        # Aggregate across all 3 windows
        longs = [t for t in trades if t["direction"] == "long"]
        shorts = [t for t in trades if t["direction"] == "short"]
        sL = stats(longs)
        sS = stats(shorts)
        out.append(f"  COMBINED long:  {fmt(sL)}")
        out.append(f"  COMBINED short: {fmt(sS)}")
        out.append("")

    # ========== Q2: ATR-at-entry buckets ==========
    out.append("")
    out.append("=" * 70)
    out.append("Q2. ATR-AT-ENTRY BUCKETS")
    out.append("=" * 70)
    out.append("For each symbol, split combined OOS trades into ATR tertiles by")
    out.append("entry ATR. Compute stats per bucket. If high-ATR or low-ATR entries")
    out.append("are consistently +/-EV, that's a pre-trade filter.")
    out.append("")

    for sym in ["XAUUSD", "NAS100", "US500", "GER40", "EURUSD"]:
        for mode in ["base", "long"]:
            trades = by_symmode.get((sym, mode), [])
            trades = [t for t in trades if t["atr"] > 0]
            if len(trades) < 30:
                continue
            atrs = [t["atr"] for t in trades]
            edges = percentile_buckets(atrs, 3)
            if edges is None:
                continue
            out.append(f"--- {sym} {mode} (ATR tertiles) ---")
            labels = ["LOW  ATR", "MID  ATR", "HIGH ATR"]
            for i in range(3):
                lo, hi = edges[i], edges[i+1]
                bucket = [t for t in trades if lo <= t["atr"] < hi]
                s = stats(bucket)
                out.append(f"  {labels[i]} [{lo:.2f}..{hi:.2f}): {fmt(s)}")
            out.append("")

    # ========== Q3: bars_held cohorts ==========
    out.append("")
    out.append("=" * 70)
    out.append("Q3. BARS_HELD COHORTS")
    out.append("=" * 70)
    out.append("Split combined OOS by bars_held into FAST(<=3 bars) / MED(4-10) /")
    out.append("SLOW(>10). If one cohort is consistently +EV across symbols, that's")
    out.append("a candidate exit-rule edge (e.g. force-exit at bar N).")
    out.append("")

    for sym in ["XAUUSD", "NAS100", "US500", "GER40", "EURUSD"]:
        for mode in ["base", "long"]:
            trades = by_symmode.get((sym, mode), [])
            if len(trades) < 30:
                continue
            fast = [t for t in trades if t["bars_held"] <= 3]
            med = [t for t in trades if 4 <= t["bars_held"] <= 10]
            slow = [t for t in trades if t["bars_held"] > 10]
            out.append(f"--- {sym} {mode} ---")
            out.append(f"  FAST (<=3):    {fmt(stats(fast))}")
            out.append(f"  MED  (4-10):   {fmt(stats(med))}")
            out.append(f"  SLOW (>10):    {fmt(stats(slow))}")
            out.append("")

    # ========== Q4: Exit reason as edge ==========
    out.append("")
    out.append("=" * 70)
    out.append("Q4. EXIT REASON BREAKDOWN (already known but framed for edge use)")
    out.append("=" * 70)
    out.append("WEEKEND-exit P&L distribution: are weekend exits lopsided per symbol?")
    out.append("If WEEKEND exits average strongly +/- per symbol, that's a session-")
    out.append("management edge (close-before-weekend vs hold-through-weekend rule).")
    out.append("")

    for sym in ["XAUUSD", "NAS100", "US500", "GER40", "EURUSD"]:
        for mode in ["base", "long"]:
            trades = by_symmode.get((sym, mode), [])
            if not trades:
                continue
            out.append(f"--- {sym} {mode} ---")
            for reason in ["TP", "SL", "WEEKEND", "EOF"]:
                bucket = [t for t in trades if t["exit_reason"] == reason]
                out.append(f"  {reason:8s}: {fmt(stats(bucket))}")
            out.append("")

    # ========== Q5: Direction within long-only mode (sanity check) ==========
    # All long-mode trades are direction=long by construction - skip.

    # ========== Q6: Day-of-week / hour-of-day ==========
    out.append("")
    out.append("=" * 70)
    out.append("Q6. DAY-OF-WEEK ENTRY (combined OOS per symbol/mode)")
    out.append("=" * 70)
    out.append("Sun=0, Mon=1, ..., Fri=5. Looking for systematically +/- days.")
    out.append("")

    for sym in ["XAUUSD", "NAS100", "US500", "GER40", "EURUSD"]:
        for mode in ["base", "long"]:
            trades = by_symmode.get((sym, mode), [])
            if len(trades) < 30:
                continue
            out.append(f"--- {sym} {mode} ---")
            for dow in range(7):
                # python weekday(): Mon=0..Sun=6
                day_trades = [t for t in trades if t["entry_ts"].weekday() == dow]
                if not day_trades:
                    continue
                day_name = ["Mon","Tue","Wed","Thu","Fri","Sat","Sun"][dow]
                out.append(f"  {day_name}: {fmt(stats(day_trades))}")
            out.append("")

    text = "\n".join(out)
    with open("edges_summary.txt", "w") as f:
        f.write(text)
    print(text)


if __name__ == "__main__":
    main()
