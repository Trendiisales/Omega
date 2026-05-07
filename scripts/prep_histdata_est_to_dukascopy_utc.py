#!/usr/bin/env python3
"""
prep_histdata_est_to_dukascopy_utc.py
======================================
One-off data prep for the FVG signal test on HistData symbols whose tick
files are published in EST (HistData's default for T_LAST/T_BID_ASK
downloads).

WHY THIS EXISTS
    HistData publishes tick CSVs as
        YYYYMMDD HHMMSSfff,bid,ask,vol
    with timestamps in EST (UTC-5, no DST per HistData convention). The
    FVG sniff test (`scripts/usdjpy_xauusd_fvg_signal_test.py`) treats
    HistData timestamps as UTC. If you fed the raw EST file in, all the
    BY SESSION buckets would be mis-labeled by 5 hours - "asian 00-07
    UTC" would actually be 05-12 UTC (London hours), etc.

    The S59 USDJPY engine deliberately treats EST as UTC for parity
    with Dukascopy XAUUSD data, but the sniff test specifically uses
    UTC-hour session bucketing as a diagnostic. So for the sniff test
    we want REAL UTC.

    Rather than modify the core test script, this prep step:
      - reads the monthly HistData ASCII files in a date window
      - shifts each timestamp EST -> UTC (+5h)
      - converts to ms-epoch (Dukascopy format) which removes any
        remaining textual-TZ ambiguity
      - emits a single header-prefixed CSV
        (`timestamp,askPrice,bidPrice` followed by ms-epoch rows)
    The FVG sniff test auto-detects the Dukascopy header and reads it
    unchanged.

INPUT LAYOUT
    --src-root contains subdirectories like
        HISTDATA_COM_ASCII_USDJPY_T202509
        HISTDATA_COM_ASCII_USDJPY_T202510
        ...
    Each subdir contains a single DAT_ASCII_*_T_*.csv file with rows in
    the format `YYYYMMDD HHMMSSfff,bid,ask,vol` (no header). Re-download
    duplicates named like `..._T202503 (1)` are SKIPPED - we only pick
    canonical dirs whose name ends in exactly 6 digits.

OUTPUT
    One CSV at --out path:
        timestamp,askPrice,bidPrice
        1756684800000,148.123000,148.121000
        ...
    Timestamps are int64 ms-epoch UTC. Prices preserve 6-decimal
    precision from the input.

USAGE EXAMPLE (USDJPY, Sep 2025 - Feb 2026 inclusive, matching the
XAUUSD sniff-test window 2025-09-01 -> 2026-03-01):
    python3 scripts/prep_histdata_est_to_dukascopy_utc.py \\
        --src-root  ~/Tick/USDJPY \\
        --pattern   "HISTDATA_COM_ASCII_USDJPY_T*" \\
        --start     2025-09 \\
        --end       2026-02 \\
        --out       ~/Tick/USDJPY/USDJPY_2025-09_2026-02_combined_UTC.csv

NOTES
    * Memory-bounded by --chunksize (default 2,000,000 rows ~ ~64 MB).
    * Months are processed in chronological order so the output is
      already sorted, matching what the FVG test's chunked loader
      expects.
    * This script does NOT modify any core test code. It is standalone
      data prep and can be re-run safely - it overwrites --out.
"""

from __future__ import annotations

import argparse
import re
import sys
import time
from pathlib import Path
from typing import List, Tuple

import pandas as pd


# Match HistData month directories whose name ends in exactly 6 digits,
# OR ends in 6 digits + " (N)" duplicate marker (Finder/HistData adds
# this when the same month is re-downloaded). When BOTH the plain dir
# and a "(N)" duplicate exist for the same month, the plain one wins;
# when only the duplicate exists, we fall back to it (otherwise we'd
# silently miss months for which only a re-download was kept).
MONTH_RE = re.compile(r"_T(\d{6})(?:\s*\(\d+\))?$")


def parse_yyyymm(s: str) -> int:
    """Accept '202509' or '2025-09'. Return int 202509."""
    s = s.replace("-", "")
    if len(s) != 6 or not s.isdigit():
        raise ValueError(f"bad YYYYMM: {s!r}")
    return int(s)


def find_month_dirs(
    src_root: Path,
    pattern: str,
    start_yyyymm: int,
    end_yyyymm: int,
) -> List[Tuple[int, Path]]:
    """Return [(yyyymm, dirpath), ...] sorted by month.

    Per-month dedup rule: if a month has both a plain
    "..._T<YYYYMM>" dir and one or more "..._T<YYYYMM> (N)" duplicates,
    the plain dir wins. If only "(N)" variants exist, the lowest-N one
    is used so the result is deterministic.
    """
    candidates: dict = {}  # yyyymm -> (sort_key, Path)
    for d in sorted(src_root.glob(pattern)):
        if not d.is_dir():
            continue
        m = MONTH_RE.search(d.name)
        if not m:
            continue
        ym = int(m.group(1))
        if not (start_yyyymm <= ym <= end_yyyymm):
            continue
        # Lower sort_key wins. Plain (no parens) -> 0; "(1)" -> 1; etc.
        # This makes plain dirs win over duplicates deterministically.
        if d.name.rstrip().endswith(")"):
            try:
                n = int(d.name.rsplit("(", 1)[1].rstrip(")").strip())
            except ValueError:
                n = 999
            sort_key = max(1, n)
        else:
            sort_key = 0
        prev = candidates.get(ym)
        if prev is None or sort_key < prev[0]:
            candidates[ym] = (sort_key, d)
    return [(ym, candidates[ym][1]) for ym in sorted(candidates)]


def find_csv_in_dir(d: Path) -> Path:
    """Find the canonical DAT_ASCII_*_T_*.csv inside a HistData month dir."""
    candidates = list(d.glob("DAT_ASCII_*_T_*.csv"))
    if not candidates:
        raise RuntimeError(f"no DAT_ASCII_*_T_*.csv inside {d}")
    if len(candidates) > 1:
        raise RuntimeError(
            f"multiple DAT_ASCII_*_T_*.csv inside {d}: "
            f"{[c.name for c in candidates]}"
        )
    return candidates[0]


def convert_one_file(
    src: Path,
    out_fh,
    rows_so_far: int,
    chunksize: int,
    shift_hours: int,
) -> int:
    """Stream `src` through pandas in chunks, shift EST -> UTC by
    `shift_hours`, write Dukascopy-format rows to the open file handle
    `out_fh`. Returns the cumulative row count including `rows_so_far`."""
    t0 = time.time()
    rows_written = 0
    bad_rows = 0
    reader = pd.read_csv(
        src,
        chunksize=chunksize,
        header=None,
        names=["ts_str", "bid", "ask", "vol"],
        dtype={"ts_str": str, "bid": float, "ask": float, "vol": float},
    )
    for chunk in reader:
        # Parse HistData EST timestamps. Note: utc=False is the
        # important bit - we're parsing as naive then shifting manually,
        # because HistData's "EST" is fixed UTC-5 (no DST), not the
        # America/New_York zone with DST.
        dt_naive = pd.to_datetime(
            chunk["ts_str"],
            format="%Y%m%d %H%M%S%f",
            utc=False,
            errors="coerce",
        )
        valid = dt_naive.notna()
        bad_in_chunk = int((~valid).sum())
        bad_rows += bad_in_chunk
        if not valid.any():
            continue
        chunk = chunk.loc[valid].reset_index(drop=True)
        dt_naive = dt_naive.loc[valid].reset_index(drop=True)
        # Shift EST -> UTC: HistData EST is UTC-5 year-round.
        dt_utc = dt_naive + pd.Timedelta(hours=shift_hours)
        # ms-epoch UTC int64
        ts_ms = (dt_utc.astype("int64") // 10**6).astype("int64")
        out_df = pd.DataFrame({
            "timestamp": ts_ms.values,
            "askPrice": chunk["ask"].values,
            "bidPrice": chunk["bid"].values,
        })
        out_df.to_csv(out_fh, header=False, index=False, float_format="%.6f")
        rows_written += len(out_df)
    elapsed = time.time() - t0
    note = f", {bad_rows:,} unparseable rows skipped" if bad_rows else ""
    print(
        f"  -> {rows_written:,} rows from {src.name}  "
        f"({elapsed:.1f}s{note})"
    )
    return rows_so_far + rows_written


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument(
        "--src-root", required=True, type=Path,
        help="Parent directory containing the monthly HistData subdirs."
    )
    ap.add_argument(
        "--pattern", required=True, type=str,
        help="Glob pattern under --src-root for month subdirs "
             "(e.g. 'HISTDATA_COM_ASCII_USDJPY_T*').",
    )
    ap.add_argument(
        "--start", required=True, type=str,
        help="Inclusive start month YYYYMM or YYYY-MM (e.g. 2025-09).",
    )
    ap.add_argument(
        "--end", required=True, type=str,
        help="Inclusive end month YYYYMM or YYYY-MM (e.g. 2026-02).",
    )
    ap.add_argument(
        "--out", required=True, type=Path,
        help="Output combined Dukascopy-format CSV path.",
    )
    ap.add_argument(
        "--shift-hours", default=5, type=int,
        help="Hours to add to each EST timestamp to get UTC. Default 5 "
             "(HistData EST is UTC-5 year-round, no DST).",
    )
    ap.add_argument(
        "--chunksize", default=2_000_000, type=int,
        help="Pandas read_csv chunksize (rows). Default 2,000,000.",
    )
    args = ap.parse_args()

    src_root = args.src_root.expanduser()
    out = args.out.expanduser()
    start_ym = parse_yyyymm(args.start)
    end_ym = parse_yyyymm(args.end)

    if not src_root.is_dir():
        print(f"[fatal] --src-root not a directory: {src_root}", file=sys.stderr)
        return 1

    months = find_month_dirs(src_root, args.pattern, start_ym, end_ym)
    if not months:
        print(
            f"[fatal] no month dirs matched in {src_root} "
            f"for pattern {args.pattern!r} in window {start_ym}..{end_ym}",
            file=sys.stderr,
        )
        return 1

    print(f"[prep] {len(months)} month(s) in window {start_ym}..{end_ym}:")
    for ym, d in months:
        print(f"  {ym}: {d.name}")
    print(f"[prep] shift_hours={args.shift_hours} (EST -> UTC)")
    print(f"[prep] chunksize={args.chunksize:,}")
    print(f"[prep] writing -> {out}")

    out.parent.mkdir(parents=True, exist_ok=True)
    t0 = time.time()
    total_rows = 0
    with open(out, "w") as out_fh:
        # Dukascopy header (the FVG test's _detect_tick_format keys on
        # this literal "timestamp" prefix to pick the right parser).
        out_fh.write("timestamp,askPrice,bidPrice\n")
        for ym, d in months:
            csv = find_csv_in_dir(d)
            print(f"[prep] {ym}: {csv}")
            total_rows = convert_one_file(
                csv, out_fh, total_rows, args.chunksize, args.shift_hours,
            )

    elapsed = time.time() - t0
    size_gb = out.stat().st_size / (1024 ** 3)
    print()
    print(f"[prep] DONE: {total_rows:,} rows -> {out}")
    print(f"[prep] elapsed: {elapsed:.1f}s")
    print(f"[prep] file size: {size_gb:.2f} GB")

    # Sanity: peek the head of the output so the FVG test's format
    # detector behavior is obvious from the run log.
    print("[prep] head of output:")
    with open(out) as f:
        for _ in range(3):
            line = f.readline()
            if not line:
                break
            print(f"  {line.rstrip()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
