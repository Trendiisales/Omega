#!/usr/bin/env python3
"""
export_tsmom_warmup.py
======================
Exports the canonical post-cut H1 bar stream to a CSV the C++
TsmomEngine.warmup_from_csv() can consume at startup.

Source: phase1/signal_discovery/bars_H1.parquet
   schema: bar_ms, open_mid, high_mid, low_mid, close_mid, tick_n, avg_spread

Output: phase1/signal_discovery/tsmom_warmup_H1.csv
   schema: bar_start_ms,open,high,low,close   (one row per H1 bar)

The C++ parser in TsmomPortfolio::warmup_from_csv()
  - skips blank lines and lines starting with '#'
  - skips any first line whose first field is not a valid integer (header)
  - tokenises on commas, expects exactly 5 fields
  - treats bar order as authoritative (must be ascending bar_start_ms)

Why H1 only?
------------
The H1 stream is sufficient to fully warm every cell in the portfolio:
  - H1 cell uses raw H1 bars
  - H2/H4/H6/D1 cells synthesise their bars from the H1 stream internally
    (strides 2/4/6/24)
This matches the live-runtime topology -- TsmomPortfolio receives a single
H1 dispatch and synthesises the rest, so the warmup feed mirrors that.

Coverage check
--------------
For each cell to be FULLY ready (closes_ + ATR), the H1 stream must contain:
  H1: lookback+1 = 21 H1 bars
  H2: 2  * (lookback+1) = 42 H1 bars
  H4: 4  * (lookback+1) = 84 H1 bars
  H6: 6  * (lookback+1) = 126 H1 bars
  D1: 24 * max(lookback+1, ATR_P+lookback+1) = 24*35 = 840 H1 bars
       (ATR14 wilder bootstrap + 21-bar lookback window)
The post-cut H1 parquet covers ~365 days (~6,000+ H1 bars after weekend
gaps), so all 5 cells warm cleanly with margin to spare.

Usage
-----
    cd /Users/jo/omega_repo
    python3 phase1/signal_discovery/export_tsmom_warmup.py

Idempotent: overwrites the output CSV every run.
"""
from __future__ import annotations
import os
import sys
import pandas as pd

HERE        = os.path.dirname(os.path.abspath(__file__))
SOURCE_PATH = os.path.join(HERE, "bars_H1.parquet")
OUT_PATH    = os.path.join(HERE, "tsmom_warmup_H1.csv")


def main() -> int:
    if not os.path.exists(SOURCE_PATH):
        sys.stderr.write(
            f"[FATAL] {SOURCE_PATH} does not exist. "
            f"Run post_cut_revalidate_all.py first to materialise it.\n"
        )
        return 2

    df = pd.read_parquet(SOURCE_PATH)

    required = {"bar_ms", "open_mid", "high_mid", "low_mid", "close_mid"}
    missing = required - set(df.columns)
    if missing:
        sys.stderr.write(
            f"[FATAL] {SOURCE_PATH} missing required columns: {sorted(missing)}\n"
        )
        return 3

    df = df[["bar_ms", "open_mid", "high_mid", "low_mid", "close_mid"]].copy()
    df = df.sort_values("bar_ms").reset_index(drop=True)
    df = df.dropna(subset=["bar_ms", "open_mid", "high_mid", "low_mid", "close_mid"])

    # Cast bar_ms to int64 in case parquet stored it as float (DuckDB sometimes
    # widens during aggregation).
    df["bar_ms"] = df["bar_ms"].astype("int64")

    df.columns = ["bar_start_ms", "open", "high", "low", "close"]

    # Format: integer ms, 5dp on prices (XAUUSD has 2dp natively but mid prices
    # picked up sub-cent precision through resampling; keep that fidelity).
    df.to_csv(
        OUT_PATH,
        index=False,
        header=True,
        float_format="%.5f",
    )

    n  = len(df)
    fb = int(df["bar_start_ms"].iloc[0])
    lb = int(df["bar_start_ms"].iloc[-1])
    span_h = (lb - fb) / 3_600_000.0

    print(f"[export_tsmom_warmup] wrote {n} H1 bars to {OUT_PATH}")
    print(f"[export_tsmom_warmup] first_ms={fb}  last_ms={lb}  span={span_h:.1f}h")
    print(f"[export_tsmom_warmup] expected cell readiness after warmup:")
    print(f"  H1 closes need 21    -- have {n} bars  -> "
          f"{'READY' if n >= 21 else 'COLD'}")
    print(f"  H2 closes need 42 H1 -- have {n} bars  -> "
          f"{'READY' if n >= 42 else 'COLD'}")
    print(f"  H4 closes need 84 H1 -- have {n} bars  -> "
          f"{'READY' if n >= 84 else 'COLD'}")
    print(f"  H6 closes need 126 H1 -- have {n} bars -> "
          f"{'READY' if n >= 126 else 'COLD'}")
    print(f"  D1 closes+ATR14 need 840 H1 -- have {n} bars -> "
          f"{'READY' if n >= 840 else 'COLD'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
