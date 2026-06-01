#!/usr/bin/env python3
"""Audit L2 capture quality: per-column non-zero fill rate on the logged
l2_ticks_<SYM>_*.csv files. Answers "is the IBKR L2 depth actually populated
and logged, or are the depth/volume columns still zeroed?"

Usage:
  py tools/audit_l2_quality.py [SYMBOL] [--logs DIR] [--rows N]
"""
from __future__ import annotations
import argparse, glob, os, sys
import pandas as pd

L2_COLS = ["l2_imb", "l2_bid_vol", "l2_ask_vol",
           "depth_bid_levels", "depth_ask_levels", "depth_events_total",
           "vpin", "micro_edge", "ewm_drift", "vol_ratio"]


def _logs_dir():
    for d in (r"C:\Omega\logs", os.path.expanduser("~/Tick/l2_xau_vps")):
        if os.path.isdir(d):
            return d
    return r"C:\Omega\logs"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("symbol", nargs="?", default="XAUUSD")
    ap.add_argument("--logs", default=_logs_dir())
    ap.add_argument("--rows", type=int, default=200000, help="tail N rows of newest file")
    a = ap.parse_args()

    paths = sorted(glob.glob(os.path.join(a.logs, f"l2_ticks_{a.symbol}_*.csv")))
    if not paths:
        print(f"no l2_ticks_{a.symbol}_*.csv in {a.logs}")
        return 1
    f = paths[-1]
    df = pd.read_csv(f).tail(a.rows)
    n = len(df)
    print(f"file : {f}")
    print(f"rows : {n:,}  (tail {a.rows:,})")
    span = (df['ts_ms'].iloc[-1] - df['ts_ms'].iloc[0]) / 1000.0 if 'ts_ms' in df else 0
    print(f"span : {span/60:.0f} min   rate={n/max(span,1):.1f} ticks/s")
    print(f"{'column':<20} {'nonzero%':>9} {'mean':>12} {'min':>10} {'max':>10}")
    print("-" * 65)
    for c in L2_COLS:
        if c not in df.columns:
            print(f"{c:<20} {'MISSING':>9}")
            continue
        s = pd.to_numeric(df[c], errors="coerce").fillna(0)
        nz = (s != 0).mean() * 100
        flag = "  <-- DEAD" if nz < 5 else ("  <-- sparse" if nz < 50 else "")
        print(f"{c:<20} {nz:>8.1f}% {s.mean():>12.4f} {s.min():>10.3f} {s.max():>10.3f}{flag}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
