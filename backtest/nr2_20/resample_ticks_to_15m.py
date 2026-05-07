#!/usr/bin/env python3
"""
resample_ticks_to_15m.py
========================

Aggregate XAUUSD tick CSVs (Dukascopy or duka_yr1 no-header style) into
15-minute OHLC bars matching the schema used by the existing
~/omega_repo/fvg_phase0/XAUUSD_15min/bars_XAUUSD_15min_*.csv files
that wf_compare.py already proved out on.

This script is INPUT-ONLY for tick CSVs. It does NOT modify any file
under ~/omega_repo. It only writes the output CSV to the path you pass
in --out.

Output schema (header + rows):
    ts_unix,open,high,low,close,tick_count,spread_mean

Where:
    ts_unix       UTC epoch SECOND of the 15-minute bar's OPEN.
    open          Mid price of the first tick whose UTC timestamp falls
                  inside [ts_unix, ts_unix+900).
    high          Max mid price of any tick in the window.
    low           Min mid price of any tick in the window.
    close         Mid price of the last tick in the window.
    tick_count    Number of ticks aggregated into this bar.
    spread_mean   Mean (ask - bid) across all ticks in the window.

`mid` is (ask + bid) / 2 throughout. Empty 15-minute windows (no ticks)
are omitted from the output, mirroring the existing 6-month CSV.

Supported input formats (auto-detected per file):

  Format A — header-prefixed (e.g. ~/Tick/duka_ticks/*.csv):
      timestamp,askPrice,bidPrice                       # 3-column variant
      timestamp_ms,ask,bid,ask_vol,bid_vol              # 5-column variant
      Column 1 is ms-since-UTC-epoch. Prices are floats.

  Format B — Dukascopy "duka_yr1" no-header CSV:
      YYYYMMDD,HH:MM:SS,bid,ask,close_or_dup,volume
      Six columns. Column[2] is the bid; column[3] is the ask.

Usage
-----
    # 2-year combined run (recommended for wf_compare; single input file)
    python3 resample_ticks_to_15m.py \\
        --in /Users/jo/Tick/duka_ticks/XAUUSD_2024-03_2026-04_combined.csv \\
        --out /Users/jo/omega_repo/fvg_phase0/XAUUSD_15min/bars_XAUUSD_15min_2024-03_2026-04.csv

    # Multi-file merge (each file already chronological internally; pass in
    # chronological filename order for non-overlapping ranges)
    python3 resample_ticks_to_15m.py \\
        --in /Users/jo/Tick/duka_yr1/XAUUSD_2023_09.csv \\
        --in /Users/jo/Tick/duka_yr1/XAUUSD_2023_10.csv \\
        --out /tmp/bars_2023_q4.csv

    # Self-test against tiny synthetic input (no I/O on real data)
    python3 resample_ticks_to_15m.py --self-test

Author: Session 2026-05-03 (continuation of NR2-20 / VWAPC walk-forward thread)
"""

import argparse
import csv
import os
import sys
from datetime import datetime, timezone

# 15-minute bars in seconds. Single source of truth.
BAR_SECONDS = 900


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _floor_to_bar(ts_seconds: int, bar_seconds: int = BAR_SECONDS) -> int:
    """Floor a UTC epoch-second timestamp to the start of its 15-minute bar."""
    return ts_seconds - (ts_seconds % bar_seconds)


def _detect_format(first_line: str) -> str:
    """Return 'A' (header-prefixed ms-epoch) or 'B' (no-header date,time,...)."""
    if not first_line:
        raise ValueError("Empty input file (no first line).")
    lower = first_line.lower()
    # Format A is identified by the presence of any of these header tokens.
    a_markers = ("timestamp", "askprice", "bidprice", "ask_vol", "bid_vol")
    if any(m in lower for m in a_markers):
        return "A"
    # Format B identification: column[0] is an 8-digit date.
    parts = first_line.strip().split(",")
    if len(parts) >= 4 and len(parts[0]) == 8 and parts[0].isdigit():
        return "B"
    raise ValueError(
        "Cannot recognise tick format from first line: "
        f"{first_line!r}. Expected either a header-prefixed file (Format A) "
        "or YYYYMMDD,HH:MM:SS,bid,ask,... lines (Format B)."
    )


def _parse_format_a(path: str):
    """Yield (ts_seconds, ask, bid) tuples for header-prefixed files."""
    with open(path, "r", newline="") as fh:
        reader = csv.reader(fh)
        try:
            header = next(reader)
        except StopIteration:
            return
        header_lower = [h.strip().lower() for h in header]
        ts_col = ask_col = bid_col = None
        for i, h in enumerate(header_lower):
            if h in ("timestamp", "timestamp_ms", "ts", "ts_ms"):
                ts_col = i
            elif h in ("askprice", "ask"):
                ask_col = i
            elif h in ("bidprice", "bid"):
                bid_col = i
        if ts_col is None or ask_col is None or bid_col is None:
            raise ValueError(
                f"File {path}: could not locate ts/ask/bid columns in "
                f"header {header!r}."
            )
        for row in reader:
            if not row:
                continue
            try:
                ts_ms = int(row[ts_col])
                ask = float(row[ask_col])
                bid = float(row[bid_col])
            except (ValueError, IndexError):
                # Skip malformed lines silently rather than abort the run.
                continue
            yield (ts_ms // 1000, ask, bid)


def _parse_format_b(path: str):
    """Yield (ts_seconds, ask, bid) tuples for Dukascopy 'yr1' no-header files.

    Lines look like: 20230927,00:00:00,1901.455,1901.745,1901.455,0
    Columns: date(YYYYMMDD), time(HH:MM:SS), bid, ask, last_or_dup, volume
    """
    with open(path, "r", newline="") as fh:
        reader = csv.reader(fh)
        for row in reader:
            if not row or len(row) < 4:
                continue
            try:
                date_s = row[0]
                time_s = row[1]
                bid = float(row[2])
                ask = float(row[3])
            except (ValueError, IndexError):
                continue
            try:
                dt = datetime.strptime(
                    f"{date_s} {time_s}", "%Y%m%d %H:%M:%S"
                ).replace(tzinfo=timezone.utc)
            except ValueError:
                continue
            yield (int(dt.timestamp()), ask, bid)


def _stream_ticks(paths):
    """Yield (ts_seconds, ask, bid) merged in input order across all paths.

    NOTE: ticks within each file are assumed already chronologically sorted.
    If you pass multiple files spanning overlapping time ranges this WILL
    NOT merge-sort them; it just concatenates. For non-overlapping monthly
    files passed in chronological filename order this is the correct
    behaviour.
    """
    for path in paths:
        with open(path, "r") as fh:
            first_line = fh.readline()
        fmt = _detect_format(first_line)
        if fmt == "A":
            yield from _parse_format_a(path)
        else:
            yield from _parse_format_b(path)


def _emit_bar(out_writer, bar_start, o, h, l, c, n, spread_sum):
    """Write one OHLC row to the output CSV with 6-decimal price formatting."""
    spread_mean = (spread_sum / n) if n > 0 else 0.0
    out_writer.writerow([
        bar_start,
        f"{o:.6f}",
        f"{h:.6f}",
        f"{l:.6f}",
        f"{c:.6f}",
        n,
        f"{spread_mean:.6f}",
    ])


# ---------------------------------------------------------------------------
# Aggregation core
# ---------------------------------------------------------------------------

def aggregate(paths, out_path):
    """Stream ticks from `paths` and write 15m bars to `out_path`.

    Returns (ticks_seen, bars_written, first_bar_ts, last_bar_ts).
    The two ts values are 0 if no bars were written.
    """
    bars_written = 0
    ticks_seen = 0
    first_bar_ts = 0
    last_bar_ts = 0

    cur_bar = None
    o = h = l = c = 0.0
    n = 0
    spread_sum = 0.0

    out_dir = os.path.dirname(out_path)
    if out_dir and not os.path.isdir(out_dir):
        os.makedirs(out_dir, exist_ok=True)

    with open(out_path, "w", newline="") as out_fh:
        out_writer = csv.writer(out_fh)
        out_writer.writerow(
            ["ts_unix", "open", "high", "low", "close",
             "tick_count", "spread_mean"]
        )

        for ts_s, ask, bid in _stream_ticks(paths):
            ticks_seen += 1
            mid = (ask + bid) / 2.0
            spread = ask - bid
            bar_start = _floor_to_bar(ts_s)

            if cur_bar is None:
                cur_bar = bar_start
                o = h = l = c = mid
                n = 1
                spread_sum = spread
                first_bar_ts = bar_start
                continue

            if bar_start == cur_bar:
                if mid > h:
                    h = mid
                if mid < l:
                    l = mid
                c = mid
                n += 1
                spread_sum += spread
            elif bar_start > cur_bar:
                _emit_bar(out_writer, cur_bar, o, h, l, c, n, spread_sum)
                bars_written += 1
                last_bar_ts = cur_bar
                cur_bar = bar_start
                o = h = l = c = mid
                n = 1
                spread_sum = spread
            else:
                # Out-of-order tick (timestamp earlier than current bar).
                # Skip rather than emit a backwards bar; safer than guessing.
                continue

        if cur_bar is not None and n > 0:
            _emit_bar(out_writer, cur_bar, o, h, l, c, n, spread_sum)
            bars_written += 1
            last_bar_ts = cur_bar

    return ticks_seen, bars_written, first_bar_ts, last_bar_ts


# ---------------------------------------------------------------------------
# Self-test
# ---------------------------------------------------------------------------

def _self_test():
    """Build a tiny synthetic file, aggregate, and assert OHLC + spread."""
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        in_path = os.path.join(tmp, "tiny.csv")
        out_path = os.path.join(tmp, "tiny_bars.csv")
        # Three ticks: first two share one 15m bar, the third lands in the
        # next bar so we exercise the bar-flush branch as well.
        with open(in_path, "w") as fh:
            fh.write("timestamp,askPrice,bidPrice\n")
            fh.write("1709258400000,2000.500,2000.000\n")  # mid=2000.250
            fh.write("1709258500000,2001.500,2001.000\n")  # mid=2001.250
            fh.write("1709259300000,2002.500,2002.000\n")  # mid=2002.250 (next bar)
        ticks, bars, first_ts, last_ts = aggregate([in_path], out_path)
        assert ticks == 3, f"expected 3 ticks, got {ticks}"
        assert bars == 2, f"expected 2 bars, got {bars}"
        assert first_ts == 1709258400, f"first_ts wrong: {first_ts}"
        assert last_ts == 1709259300, f"last_ts wrong: {last_ts}"
        with open(out_path) as fh:
            rows = list(csv.reader(fh))
        assert rows[0] == [
            "ts_unix", "open", "high", "low", "close",
            "tick_count", "spread_mean"
        ]
        bar1 = rows[1]
        assert int(bar1[0]) == 1709258400
        assert float(bar1[1]) == 2000.250
        assert float(bar1[2]) == 2001.250
        assert float(bar1[3]) == 2000.250
        assert float(bar1[4]) == 2001.250
        assert int(bar1[5]) == 2
        assert abs(float(bar1[6]) - 0.5) < 1e-9
        bar2 = rows[2]
        assert int(bar2[0]) == 1709259300
        assert float(bar2[1]) == 2002.250
        assert float(bar2[4]) == 2002.250
        assert int(bar2[5]) == 1
        print("Self-test passed: 3 ticks -> 2 bars with correct OHLC, "
              "tick_count, and spread_mean.")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv=None):
    p = argparse.ArgumentParser(
        description=(
            "Resample XAUUSD tick CSVs into 15-minute OHLC bar CSV "
            "matching the ~/omega_repo/fvg_phase0/XAUUSD_15min schema."
        )
    )
    p.add_argument(
        "--in", dest="inputs", action="append", default=[],
        help=("Input tick CSV. Pass --in once per file. Files are concatenated "
              "in argument order; assume each file is internally sorted."),
    )
    p.add_argument(
        "--out", dest="out", default=None,
        help="Output 15m-bar CSV path. Will be overwritten if it exists.",
    )
    p.add_argument(
        "--self-test", action="store_true",
        help="Run the built-in synthetic test instead of processing input.",
    )
    args = p.parse_args(argv)

    if args.self_test:
        _self_test()
        return 0

    if not args.inputs or not args.out:
        p.error("--in (one or more) and --out are required unless --self-test is set.")

    for path in args.inputs:
        if not os.path.isfile(path):
            print(f"ERROR: input file not found: {path}", file=sys.stderr)
            return 2

    print(
        f"Aggregating {len(args.inputs)} tick file(s) -> 15m bars at "
        f"{args.out}",
        file=sys.stderr,
    )
    ticks, bars, first_ts, last_ts = aggregate(args.inputs, args.out)
    if bars > 0:
        first_iso = datetime.fromtimestamp(first_ts, tz=timezone.utc).isoformat()
        last_iso = datetime.fromtimestamp(last_ts, tz=timezone.utc).isoformat()
        print(
            f"Done. ticks_read={ticks:,}  bars_written={bars:,}  "
            f"span={first_iso} -> {last_iso}",
            file=sys.stderr,
        )
    else:
        print(
            f"Done. ticks_read={ticks:,}  bars_written=0 (no bars emitted).",
            file=sys.stderr,
        )
    print(f"Output: {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
