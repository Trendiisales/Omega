#!/usr/bin/env python3
"""
scripts/histdata_to_blackbull.py

Convert HistData tick CSVs (EST = fixed UTC-5, no DST) to BlackBull-compatible
per-UTC-day tick CSVs that the honest_backtest_xauusd_v2 harness can ingest.

Input:
  $TICK_ROOT/<PAIR>/HISTDATA_COM_ASCII_<PAIR>_T<YYYYMM>/
                   /DAT_ASCII_<PAIR>_T_<YYYYMM>.csv
  Format per line: "YYYYMMDD HHMMSSfff,bid,ask,vol"
  (HistData docs: timestamps are EST = UTC-5, fixed, no daylight saving.
   Verified empirically: EURUSD March 2025 first row "20250302 170000461" =
   Sunday 17:00 EST = Sunday 22:00 UTC = standard FX week-open.)

Output:
  $OUT_ROOT/histdata_<pair_lower>_daily/<YYYY-MM-DD>.csv  (UTC-day grouping)
  Format per line: "ts_ms,bid,ask"
  (Matches outputs/duka_xauusd_daily/ convention used by the harness.)

Usage:
  python3 scripts/histdata_to_blackbull.py PAIR [MONTH...]

Examples:
  python3 scripts/histdata_to_blackbull.py EURUSD
  python3 scripts/histdata_to_blackbull.py USDJPY 202503 202504

Env vars:
  TICK_ROOT  override input root (default /Users/jo/Tick)
  OUT_ROOT   override output root (default outputs/)

Performance notes:
  - Day-cached parsing (calendar.timegm called once per UTC day, not per tick).
  - Buffered writes (one writelines per output file change).
  - Manual UTC day-key formatting from integer division (no datetime round-trip).
  - Benchmark: ~7-9 s per HistData month (~2.4M ticks) on the workspace VM.

Behaviour:
  - Months processed in chronological order. Each new UTC day overwrites
    its output file the first time it's seen this run; subsequent writes
    to the same day (e.g. when a UTC day spans two monthly files at the
    EST/UTC boundary) append. So a cross-month UTC day collects ticks
    from BOTH monthly files correctly.
  - Days from previous runs that aren't touched this run are left alone.
  - Skips zip files and "(1)"-suffixed duplicate dirs.
  - Tolerates malformed lines (skips them silently, counted in summary).
"""

import os
import sys
import calendar
import datetime as dt

# HistData EST is fixed UTC-5 year-round (no DST). To convert an EST clock
# reading to a UTC epoch: parse the digits AS IF they were UTC, then add
# 5 hours (because EST 12:00 = UTC 17:00 = 5h LATER on the wall clock).
EST_TO_UTC_OFFSET_SEC = 5 * 3600
SEC_PER_DAY           = 86400
EPOCH_DATE            = dt.date(1970, 1, 1)

TICK_ROOT_DEFAULT = "/Users/jo/Tick"
OUT_ROOT_DEFAULT  = "outputs"


def find_monthly_csvs(pair_dir, requested_months):
    """Return sorted list of (yyyymm, full_csv_path)."""
    out = []
    for sub in sorted(os.listdir(pair_dir)):
        full = os.path.join(pair_dir, sub)
        if not os.path.isdir(full):
            continue
        # Skip "(1)"-style duplicates
        if "(" in sub:
            continue
        if "_T" not in sub:
            continue
        ym = sub.rsplit("_T", 1)[-1]
        if requested_months and ym not in requested_months:
            continue
        inner = None
        for f in os.listdir(full):
            if f.endswith(".csv") and f.startswith("DAT_ASCII_"):
                inner = os.path.join(full, f)
                break
        if inner is None:
            continue
        out.append((ym, inner))
    out.sort(key=lambda t: t[0])
    return out


def utc_day_key_from_days(day_index):
    """day_index = epoch days. Return 'YYYY-MM-DD'."""
    d = EPOCH_DATE + dt.timedelta(days=day_index)
    return d.isoformat()


def main():
    if len(sys.argv) < 2:
        sys.exit(
            "usage: histdata_to_blackbull.py PAIR [MONTH...]\n"
            "  PAIR     e.g. EURUSD GBPUSD USDJPY USDCAD AUDUSD NZDUSD\n"
            "  MONTH    e.g. 202503 (optional; default = all available)\n"
        )
    pair      = sys.argv[1].upper()
    requested = set(sys.argv[2:]) if len(sys.argv) > 2 else None

    tick_root = os.environ.get("TICK_ROOT", TICK_ROOT_DEFAULT)
    out_root  = os.environ.get("OUT_ROOT",  OUT_ROOT_DEFAULT)

    pair_dir = os.path.join(tick_root, pair)
    if not os.path.isdir(pair_dir):
        sys.exit(f"[FATAL] no such pair dir: {pair_dir}")

    out_dir = os.path.join(out_root, f"histdata_{pair.lower()}_daily")
    os.makedirs(out_dir, exist_ok=True)

    csvs = find_monthly_csvs(pair_dir, requested)
    if not csvs:
        sys.exit(f"[FATAL] no monthly CSVs found in {pair_dir} "
                 f"(requested={requested})")

    print(f"[info] {pair}: processing {len(csvs)} monthly file(s) -> {out_dir}",
          flush=True)

    # Per-EST-date cache: avoid calendar.timegm per tick.
    # Maps "YYYYMMDD" -> base UTC epoch seconds for 00:00:00 UTC of that
    # EST date (= naive_secs interpreted as UTC, then we add the 5h offset
    # at use-time when constructing the UTC ms).
    est_date_to_naive_day_secs = {}

    seen_days   = set()  # UTC day_key strings opened-fresh this run
    fh_out      = None
    last_day_ms = None   # epoch-day index of currently open output file
    last_day_key = ""
    total_ticks = 0
    total_days  = 0
    skipped     = 0

    write_buf   = []     # list of strings buffered for the current day file
    BUF_FLUSH_N = 50000  # flush buffer every N ticks (kept on same day)

    def flush_buf():
        nonlocal write_buf
        if fh_out is not None and write_buf:
            fh_out.writelines(write_buf)
            write_buf = []

    try:
        for ym, csv_path in csvs:
            # End previous month: flush + close any open handle
            flush_buf()
            if fh_out is not None:
                fh_out.close()
                fh_out = None
                last_day_ms = None
                last_day_key = ""
            print(f"[info]   {ym}: reading {os.path.basename(csv_path)}",
                  flush=True)
            with open(csv_path, "r") as f:
                for line in f:
                    # Lightweight parse: split on first two commas only.
                    # Format: "YYYYMMDD HHMMSSfff,bid,ask,vol"
                    if not line:
                        continue
                    # Find positions of first two commas
                    c1 = line.find(",")
                    if c1 < 0:
                        skipped += 1
                        continue
                    c2 = line.find(",", c1 + 1)
                    if c2 < 0:
                        skipped += 1
                        continue
                    c3 = line.find(",", c2 + 1)
                    if c3 < 0:
                        # No vol field; allow it (use rest as ask)
                        ts_str = line[:c1]
                        bid    = line[c1 + 1:c2]
                        ask    = line[c2 + 1:].rstrip("\r\n")
                    else:
                        ts_str = line[:c1]
                        bid    = line[c1 + 1:c2]
                        ask    = line[c2 + 1:c3]
                    if len(ts_str) < 18:
                        skipped += 1
                        continue
                    # Parse timestamp WITHOUT building a datetime per tick.
                    date_key = ts_str[0:8]
                    base_naive_day_secs = est_date_to_naive_day_secs.get(date_key)
                    if base_naive_day_secs is None:
                        try:
                            yyyy = int(date_key[0:4])
                            mm   = int(date_key[4:6])
                            dd   = int(date_key[6:8])
                            base_naive_day_secs = calendar.timegm(
                                (yyyy, mm, dd, 0, 0, 0, 0, 0, 0))
                            est_date_to_naive_day_secs[date_key] = base_naive_day_secs
                        except Exception:
                            skipped += 1
                            continue
                    try:
                        hh  = int(ts_str[9:11])
                        mi  = int(ts_str[11:13])
                        ss  = int(ts_str[13:15])
                        fff = int(ts_str[15:18])
                    except Exception:
                        skipped += 1
                        continue
                    naive_secs = base_naive_day_secs + hh * 3600 + mi * 60 + ss
                    utc_secs   = naive_secs + EST_TO_UTC_OFFSET_SEC
                    ts_ms      = utc_secs * 1000 + fff
                    day_idx    = utc_secs // SEC_PER_DAY
                    if day_idx != last_day_ms:
                        # Day rolled; flush + reopen
                        flush_buf()
                        if fh_out is not None:
                            fh_out.close()
                        last_day_key = utc_day_key_from_days(day_idx)
                        out_path = os.path.join(out_dir, f"{last_day_key}.csv")
                        if last_day_key not in seen_days:
                            fh_out = open(out_path, "w")
                            fh_out.write("ts_ms,bid,ask\n")
                            seen_days.add(last_day_key)
                            total_days += 1
                        else:
                            fh_out = open(out_path, "a")
                        last_day_ms = day_idx
                    write_buf.append(f"{ts_ms},{bid},{ask}\n")
                    total_ticks += 1
                    if len(write_buf) >= BUF_FLUSH_N:
                        flush_buf()
            # End-of-file: flush leftover
            flush_buf()
    finally:
        flush_buf()
        if fh_out is not None:
            fh_out.close()

    print(f"[done] {pair}: {total_ticks:,} ticks across {total_days} UTC days "
          f"({skipped} malformed lines skipped)", flush=True)


if __name__ == "__main__":
    main()
