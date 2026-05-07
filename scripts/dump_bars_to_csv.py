#!/usr/bin/env python3
"""
dump_bars_to_csv.py
===================

One-shot helper for the synthetic-trace verifier
(backtest/verify_xauusd_fvg.cpp). Reads a bar pickle written by the
fvg_phase0 / fvg_pnl_backtest_v3 pipelines and writes a flat CSV the
C++ verifier can ingest without a Python binding.

Usage
-----
    cd ~/omega_repo
    python3 scripts/dump_bars_to_csv.py \
        fvg_phase0/XAUUSD_15min/bars_XAUUSD_15min_2025-09-01_2026-03-01.pkl

By default writes <stem>.csv next to the input file, i.e.
    fvg_phase0/XAUUSD_15min/bars_XAUUSD_15min_2025-09-01_2026-03-01.csv

Output columns (order is fixed; the verifier parses positionally):
    ts_unix        int64    bar START time, unix seconds, UTC
    open           float    bar open price
    high           float    bar high
    low            float    bar low
    close          float    bar close
    tick_count     int      number of ticks during the bar
    spread_mean    float    mean bid-ask spread during the bar (price units)

Why pre-convert at all
----------------------
Pickle is a Python serialisation format with no canonical C++ reader.
Pre-flattening keeps the verifier dependency-free (no pybind, no embed,
no shell-out to Python from C++).
"""

from __future__ import annotations

import sys
from pathlib import Path

import pandas as pd


def main() -> int:
    if len(sys.argv) < 2 or len(sys.argv) > 3:
        print("usage: dump_bars_to_csv.py <bars.pkl> [out.csv]",
              file=sys.stderr)
        return 2

    pkl_path = Path(sys.argv[1]).resolve()
    if not pkl_path.exists():
        print(f"[error] not found: {pkl_path}", file=sys.stderr)
        return 2

    out_path = (
        Path(sys.argv[2]).resolve()
        if len(sys.argv) == 3
        else pkl_path.with_suffix(".csv")
    )

    bars = pd.read_pickle(pkl_path)

    needed = {"open", "high", "low", "close", "tick_count", "spread_mean"}
    missing = needed - set(bars.columns)
    if missing:
        print(f"[error] missing columns in {pkl_path}: {sorted(missing)}",
              file=sys.stderr)
        print(f"[error] available columns: {list(bars.columns)}",
              file=sys.stderr)
        return 2

    if not isinstance(bars.index, pd.DatetimeIndex):
        print(f"[error] expected DatetimeIndex, got {type(bars.index)}",
              file=sys.stderr)
        return 2

    idx = bars.index
    if idx.tz is None:
        # The fvg_phase0 pipeline writes tz-aware UTC; if a pickle slipped
        # through tz-naive, treat it as UTC rather than guessing local time.
        idx = idx.tz_localize("UTC")
    else:
        idx = idx.tz_convert("UTC")

    ts_unix = (idx.view("int64") // 1_000_000_000).astype("int64")

    out = pd.DataFrame({
        "ts_unix":     ts_unix,
        "open":        bars["open"].astype(float),
        "high":        bars["high"].astype(float),
        "low":         bars["low"].astype(float),
        "close":       bars["close"].astype(float),
        "tick_count":  bars["tick_count"].astype(int),
        "spread_mean": bars["spread_mean"].astype(float),
    })
    out.dropna(subset=["open", "high", "low", "close"], inplace=True)

    out.to_csv(out_path, index=False, float_format="%.6f")
    print(f"[ok] wrote {len(out):,} bars -> {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
