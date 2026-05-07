#!/usr/bin/env python3
"""
combine_dukascopy_monthly.py
=============================
Combine the monthly tick CSVs that `download_dukascopy.py` produces into
a single CSV in the 3-column Dukascopy format the FVG sniff test
auto-detects.

WHY
    download_dukascopy.py writes files like
        SYMBOL_YYYY_MM.csv
    with columns
        timestamp_ms,ask,bid,ask_vol,bid_vol
    but the FVG sniff test (`scripts/usdjpy_xauusd_fvg_signal_test.py`)
    expects the 3-column Dukascopy format
        timestamp,askPrice,bidPrice
    The combined XAUUSD file we use for that test was built ad-hoc;
    this script reproduces that step explicitly so any new symbol
    (USATECHIDXUSD, EURUSD via Dukascopy, etc.) can be combined with
    one command.

INPUT LAYOUT
    Monthly CSV files at <src-dir>/<symbol>_YYYY_MM.csv. The script
    reads each file in chronological order, drops the two volume
    columns, and rewrites the header to match Dukascopy's expected
    schema. Months that don't have a file are SKIPPED with a warning
    (typical when a download was incomplete).

USAGE
    python3 scripts/combine_dukascopy_monthly.py \\
        --src-dir ~/Tick/duka_ticks \\
        --symbol  USATECHIDXUSD \\
        --start   2025-09 \\
        --end     2026-02 \\
        --out     ~/Tick/duka_ticks/USATECHIDXUSD_2025-09_2026-02_combined.csv

NOTES
    * Streams line-by-line (constant memory regardless of file size).
    * Re-runnable; overwrites --out.
    * Does NOT verify .done markers - that's the wrapper's job.
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path
from typing import Iterator, List, Tuple


def parse_yyyymm(s: str) -> Tuple[int, int]:
    """Accept '202509' or '2025-09'. Return (year, month)."""
    s = s.replace("-", "")
    if len(s) != 6 or not s.isdigit():
        raise ValueError(f"bad YYYYMM: {s!r}")
    return int(s[:4]), int(s[4:])


def iter_months(start: Tuple[int, int], end: Tuple[int, int]) -> Iterator[Tuple[int, int]]:
    """Inclusive month iterator from start (year, month) to end (year, month)."""
    y, m = start
    ey, em = end
    while (y, m) <= (ey, em):
        yield y, m
        if m == 12:
            y, m = y + 1, 1
        else:
            m += 1


def combine_one(src: Path, out_fh, header_written: bool) -> Tuple[int, bool]:
    """Stream `src` to `out_fh`, dropping the two volume columns and
    rewriting the header on first call. Returns (rows_written, header_written)."""
    rows = 0
    with open(src, "r", encoding="utf-8") as f:
        first = f.readline().strip()
        # Sanity check the input header matches what download_dukascopy.py writes
        expected = "timestamp_ms,ask,bid,ask_vol,bid_vol"
        if first != expected:
            raise RuntimeError(
                f"unexpected header in {src.name}\n"
                f"  expected: {expected}\n"
                f"  got:      {first}"
            )
        if not header_written:
            out_fh.write("timestamp,askPrice,bidPrice\n")
            header_written = True
        for line in f:
            line = line.rstrip("\n")
            if not line:
                continue
            # We could split-and-rejoin, but for a 5-col line we know
            # the structure; just take the first 3 fields by string ops.
            parts = line.split(",", 3)
            if len(parts) < 3:
                continue
            ts, ask, bid = parts[0], parts[1], parts[2]
            out_fh.write(f"{ts},{ask},{bid}\n")
            rows += 1
    return rows, header_written


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--src-dir", required=True, type=Path,
                    help="Directory containing the monthly SYMBOL_YYYY_MM.csv files.")
    ap.add_argument("--symbol", required=True, type=str,
                    help="Symbol prefix (e.g. USATECHIDXUSD, XAUUSD).")
    ap.add_argument("--start", required=True, type=str,
                    help="Inclusive start month YYYYMM or YYYY-MM.")
    ap.add_argument("--end", required=True, type=str,
                    help="Inclusive end month YYYYMM or YYYY-MM.")
    ap.add_argument("--out", required=True, type=Path,
                    help="Combined output CSV path.")
    args = ap.parse_args()

    src_dir = args.src_dir.expanduser()
    out = args.out.expanduser()
    if not src_dir.is_dir():
        print(f"[fatal] --src-dir not a directory: {src_dir}", file=sys.stderr)
        return 1

    start = parse_yyyymm(args.start)
    end = parse_yyyymm(args.end)
    months = list(iter_months(start, end))
    if not months:
        print(f"[fatal] empty month range {args.start}..{args.end}", file=sys.stderr)
        return 1

    print(f"[combine] {len(months)} month(s) {args.start}..{args.end} for {args.symbol}")
    print(f"[combine] src: {src_dir}")
    print(f"[combine] out: {out}")

    out.parent.mkdir(parents=True, exist_ok=True)
    t0 = time.time()
    total = 0
    missing: List[str] = []
    header_written = False

    with open(out, "w", encoding="utf-8") as out_fh:
        for y, m in months:
            src = src_dir / f"{args.symbol}_{y}_{m:02d}.csv"
            if not src.exists():
                missing.append(f"{y}-{m:02d}")
                print(f"  {y}-{m:02d}: MISSING ({src.name})")
                continue
            t1 = time.time()
            rows, header_written = combine_one(src, out_fh, header_written)
            total += rows
            print(f"  {y}-{m:02d}: {rows:>10,} rows from {src.name}  "
                  f"({time.time()-t1:.1f}s)")

    elapsed = time.time() - t0
    size_mb = out.stat().st_size / (1024 * 1024)
    print()
    print(f"[combine] DONE: {total:,} rows -> {out}")
    print(f"[combine] elapsed: {elapsed:.1f}s  size: {size_mb:.1f} MB")
    if missing:
        print(f"[combine] WARNING: {len(missing)} month(s) missing: "
              f"{', '.join(missing)}")
        print(f"[combine]          re-run download_dukascopy.py to fill them")
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
