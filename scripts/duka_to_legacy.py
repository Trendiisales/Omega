#!/usr/bin/env python3
"""
duka_to_legacy.py
-----------------
Convert Dukascopy XAU combined CSV into the legacy tick format expected by
backtest/htf_bt_minimal.cpp (the H4 Donchian breakout sweep harness).

Input format (Dukascopy combined):
    header row: "timestamp,askPrice,bidPrice"
    data rows:  "1709258400133,2044.562,2044.265"
                 ^ts_ms        ^ask     ^bid

Output format (htf_bt_minimal.cpp):
    no header
    data rows:  "20240301,02:00:00,2044.265,2044.562"
                 ^YYYYMMDD ^HH:MM:SS ^bid     ^ask

Notes:
    - Sub-second precision is lost (legacy parser is second-resolution).
      Acceptable: H4 strategy operates on 4-hour bars; intra-second order
      does not affect bar reconstruction.
    - Second-level date computation is cached: most ticks share the same
      second, so we only recompute the date string when ts_sec changes.
      ~10x speedup on high-frequency XAU data.

Usage:
    python3 duka_to_legacy.py INPUT_DUKA.csv OUTPUT_LEGACY.csv

Memory:
    Constant (line-streamed). Disk: output is roughly the same size as
    input (~5 GB for the 2024-03_2026-04 combined file).
"""
from __future__ import annotations

import sys
import time
from datetime import datetime, timezone


def convert(input_path: str, output_path: str) -> None:
    t0 = time.monotonic()
    rows_in = 0
    rows_out = 0
    rows_skipped = 0

    last_sec = -1
    last_date_str = ""

    # Large buffers to keep I/O efficient on 5 GB scale.
    with open(input_path, "r", buffering=1 << 22) as fin, \
         open(output_path, "w", buffering=1 << 22) as fout:

        # Skip header
        header = fin.readline()
        if not header.startswith("timestamp"):
            print(
                f"WARNING: expected header starting with 'timestamp', got: {header[:80]!r}",
                file=sys.stderr,
            )

        for line in fin:
            rows_in += 1

            # Hand-parse for speed: two comma offsets, three slices.
            i1 = line.find(",")
            i2 = line.find(",", i1 + 1) if i1 >= 0 else -1
            if i1 < 0 or i2 < 0:
                rows_skipped += 1
                continue

            ts_str = line[:i1]
            ask = line[i1 + 1 : i2]
            bid = line[i2 + 1 :].rstrip("\r\n")

            try:
                ts_ms = int(ts_str)
            except ValueError:
                rows_skipped += 1
                continue

            ts_sec = ts_ms // 1000

            # Date string caching: only recompute when second changes.
            if ts_sec != last_sec:
                dt = datetime.fromtimestamp(ts_sec, tz=timezone.utc)
                last_date_str = (
                    f"{dt.year:04d}{dt.month:02d}{dt.day:02d},"
                    f"{dt.hour:02d}:{dt.minute:02d}:{dt.second:02d}"
                )
                last_sec = ts_sec

            # Write: YYYYMMDD,HH:MM:SS,bid,ask (note: bid before ask)
            fout.write(f"{last_date_str},{bid},{ask}\n")
            rows_out += 1

            if rows_out % 10_000_000 == 0:
                elapsed = time.monotonic() - t0
                rate = rows_out / elapsed if elapsed > 0 else 0
                print(
                    f"  {rows_out:>12,} rows converted "
                    f"({elapsed:6.1f}s, {rate:,.0f} rows/s)",
                    file=sys.stderr,
                )

    elapsed = time.monotonic() - t0
    rate = rows_out / elapsed if elapsed > 0 else 0
    print("", file=sys.stderr)
    print(
        f"DONE. rows_in={rows_in:,} rows_out={rows_out:,} skipped={rows_skipped:,}",
        file=sys.stderr,
    )
    print(f"      elapsed={elapsed:.1f}s rate={rate:,.0f} rows/s", file=sys.stderr)
    print(f"      output={output_path}", file=sys.stderr)


def main() -> None:
    if len(sys.argv) != 3:
        print("Usage: duka_to_legacy.py INPUT_DUKA.csv OUTPUT_LEGACY.csv", file=sys.stderr)
        sys.exit(2)
    convert(sys.argv[1], sys.argv[2])


if __name__ == "__main__":
    main()
