#!/usr/bin/env python3
"""
Dukascopy XAUUSD tick data downloader.
Downloads 2 years: 2024-03-01 to 2026-03-01.
Output: CSV per month in ./duka_ticks/XAUUSD_YYYY_MM.csv
Format: timestamp_ms,ask,bid,ask_vol,bid_vol

Usage:
    python3 download_dukascopy.py [--symbol XAUUSD] [--start 2024-03-01] [--end 2026-03-01]
    python3 download_dukascopy.py --resume   # skip already-complete months
"""

import urllib.request
import struct
import lzma
import datetime
import os
import sys
import time
import argparse
import csv

# ── Constants ─────────────────────────────────────────────────────────────────
BASE_URL    = "https://datafeed.dukascopy.com/datafeed"
TICK_BYTES  = 20        # each tick record is 20 bytes
PRICE_DIV   = 1000.0    # XAUUSD: raw int / 1000 = price in USD
OUT_DIR     = "./duka_ticks"
RETRY_MAX   = 5
RETRY_DELAY = 3.0       # seconds between retries
HOUR_DELAY  = 0.15      # seconds between hour requests (be polite)
UA          = "Mozilla/5.0 (compatible; tick-downloader/1.0)"

# ── Helpers ───────────────────────────────────────────────────────────────────

def fetch_hour(symbol: str, year: int, month: int, day: int, hour: int) -> bytes:
    """Fetch one hour of bi5 data. Returns raw bytes (may be empty = no ticks)."""
    # Dukascopy month is 0-indexed
    url = f"{BASE_URL}/{symbol}/{year}/{month-1:02d}/{day:02d}/{hour:02d}h_ticks.bi5"
    for attempt in range(RETRY_MAX):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": UA})
            with urllib.request.urlopen(req, timeout=20) as resp:
                return resp.read()
        except urllib.error.HTTPError as e:
            if e.code == 404:
                return b""   # No data for this hour -- normal
            if e.code in (503, 429):
                wait = RETRY_DELAY * (attempt + 1)
                print(f"    [{e.code}] backing off {wait:.0f}s (attempt {attempt+1})", flush=True)
                time.sleep(wait)
            else:
                print(f"    HTTP {e.code} on {url}", flush=True)
                return b""
        except Exception as e:
            wait = RETRY_DELAY * (attempt + 1)
            print(f"    Error {e} -- retry in {wait:.0f}s", flush=True)
            time.sleep(wait)
    print(f"    GAVE UP after {RETRY_MAX} attempts: {url}", flush=True)
    return b""


def decompress_bi5(data: bytes) -> list:
    """Decompress bi5 LZMA data and return list of (ms_offset, ask, bid, ask_vol, bid_vol)."""
    if not data:
        return []
    try:
        raw = lzma.decompress(data)
    except Exception:
        return []
    ticks = []
    n = len(raw) // TICK_BYTES
    for i in range(n):
        off = i * TICK_BYTES
        ms_off, ask_raw, bid_raw = struct.unpack_from(">III", raw, off)
        ask_vol, bid_vol         = struct.unpack_from(">ff",  raw, off + 12)
        ask = ask_raw / PRICE_DIV
        bid = bid_raw / PRICE_DIV
        ticks.append((ms_off, ask, bid, float(ask_vol), float(bid_vol)))
    return ticks


def month_csv_path(symbol: str, year: int, month: int) -> str:
    return os.path.join(OUT_DIR, f"{symbol}_{year}_{month:02d}.csv")


def month_done_path(symbol: str, year: int, month: int) -> str:
    return os.path.join(OUT_DIR, f".done_{symbol}_{year}_{month:02d}")


def download_month(symbol: str, year: int, month: int) -> int:
    """Download one full month. Returns total tick count written."""
    csv_path  = month_csv_path(symbol, year, month)
    done_path = month_done_path(symbol, year, month)

    # Compute days in month
    if month == 12:
        next_month = datetime.date(year + 1, 1, 1)
    else:
        next_month = datetime.date(year, month + 1, 1)
    first_day  = datetime.date(year, month, 1)
    days_in_month = (next_month - first_day).days

    total_ticks = 0
    with open(csv_path, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["timestamp_ms", "ask", "bid", "ask_vol", "bid_vol"])

        for day in range(1, days_in_month + 1):
            day_ticks = 0
            for hour in range(24):
                raw = fetch_hour(symbol, year, month, day, hour)
                ticks = decompress_bi5(raw)

                if ticks:
                    # Compute absolute ms timestamp for each tick
                    base_dt = datetime.datetime(year, month, day, hour, 0, 0,
                                                tzinfo=datetime.timezone.utc)
                    base_ms = int(base_dt.timestamp() * 1000)
                    for ms_off, ask, bid, av, bv in ticks:
                        writer.writerow([base_ms + ms_off,
                                         f"{ask:.3f}", f"{bid:.3f}",
                                         f"{av:.2f}", f"{bv:.2f}"])
                    day_ticks += len(ticks)

                time.sleep(HOUR_DELAY)

            total_ticks += day_ticks
            print(f"    {year}-{month:02d}-{day:02d}: {day_ticks:7,} ticks  (month total: {total_ticks:,})",
                  flush=True)

    # Mark complete
    with open(done_path, "w") as f:
        f.write(str(total_ticks))

    return total_ticks


def iter_months(start: datetime.date, end: datetime.date):
    """Yield (year, month) tuples from start month to end month (exclusive)."""
    cur = datetime.date(start.year, start.month, 1)
    end_m = datetime.date(end.year, end.month, 1)
    while cur < end_m:
        yield cur.year, cur.month
        if cur.month == 12:
            cur = datetime.date(cur.year + 1, 1, 1)
        else:
            cur = datetime.date(cur.year, cur.month + 1, 1)


# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="Download Dukascopy tick data")
    parser.add_argument("--symbol", default="XAUUSD")
    parser.add_argument("--start",  default="2024-03-01")
    parser.add_argument("--end",    default="2026-03-01")
    parser.add_argument("--resume", action="store_true",
                        help="Skip months that already have a .done marker")
    args = parser.parse_args()

    symbol = args.symbol
    start  = datetime.date.fromisoformat(args.start)
    end    = datetime.date.fromisoformat(args.end)

    os.makedirs(OUT_DIR, exist_ok=True)

    months = list(iter_months(start, end))
    print(f"Downloading {symbol} ticks: {start} -> {end}")
    print(f"Total months: {len(months)}")
    print(f"Output dir:   {os.path.abspath(OUT_DIR)}")
    print(f"Estimated size: ~{len(months) * 150:.0f} MB uncompressed\n")

    grand_total = 0
    for idx, (year, month) in enumerate(months, 1):
        done_path = month_done_path(symbol, year, month)
        if args.resume and os.path.exists(done_path):
            with open(done_path) as f:
                n = int(f.read().strip())
            print(f"[{idx:3d}/{len(months)}] {year}-{month:02d} SKIPPED (already done, {n:,} ticks)",
                  flush=True)
            grand_total += n
            continue

        print(f"[{idx:3d}/{len(months)}] {year}-{month:02d} downloading...", flush=True)
        t0 = time.time()
        n  = download_month(symbol, year, month)
        elapsed = time.time() - t0
        grand_total += n
        print(f"  -> {n:,} ticks in {elapsed:.0f}s  |  grand total: {grand_total:,}\n",
              flush=True)

    print(f"\nDone. Grand total ticks: {grand_total:,}")
    print(f"Files in {OUT_DIR}:")
    for fname in sorted(os.listdir(OUT_DIR)):
        if fname.endswith(".csv"):
            size = os.path.getsize(os.path.join(OUT_DIR, fname))
            print(f"  {fname}  {size/1024/1024:.1f} MB")


if __name__ == "__main__":
    main()
