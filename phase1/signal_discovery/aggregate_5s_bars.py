#!/usr/bin/env python3
"""
aggregate_5s_bars.py
====================
Aggregate one monthly XAUUSD tick CSV (timestamp_ms, askPrice, bidPrice) into
5-second OHLC bars on the bid+ask mid, with tick count and spread stats per
bar.  Single duckdb pass per month so each invocation fits inside the 45s
sandbox timeout.

Usage:
    aggregate_5s_bars.py <YYYY_MM> [<YYYY_MM> ...]
    aggregate_5s_bars.py --merge

Per-month output: phase1/signal_discovery/bars_5s_YYYY_MM.parquet
Merged output:    phase1/signal_discovery/bars_5s.parquet  (concat all months)

Schema: bar_ms BIGINT, open_mid/high_mid/low_mid/close_mid DOUBLE,
        tick_n BIGINT, avg_spread/max_spread DOUBLE
"""
from __future__ import annotations
import os, sys, glob, time
import duckdb

TICK_DIR = "/sessions/hopeful-friendly-meitner/mnt/duka_ticks"
OUT_DIR  = "/sessions/hopeful-friendly-meitner/mnt/omega_repo/phase1/signal_discovery"
MERGED   = os.path.join(OUT_DIR, "bars_5s.parquet")

os.makedirs(OUT_DIR, exist_ok=True)


def aggregate_month(yyyy_mm: str) -> None:
    src = f"{TICK_DIR}/XAUUSD_{yyyy_mm}.csv"
    out = f"{OUT_DIR}/bars_5s_{yyyy_mm}.parquet"
    if not os.path.exists(src):
        print(f"SKIP {yyyy_mm}: source missing ({src})", flush=True)
        return
    if os.path.exists(out) and os.path.getsize(out) > 0:
        print(f"SKIP {yyyy_mm}: already done ({out}, {os.path.getsize(out)/1e6:.1f}MB)", flush=True)
        return
    t0 = time.time()
    con = duckdb.connect()
    con.execute("PRAGMA threads=4")
    # Per-month tick CSVs use schema: timestamp_ms,ask,bid,ask_vol,bid_vol
    con.execute(f"""
      COPY (
        SELECT
          CAST((timestamp_ms / 5000) AS BIGINT) * 5000              AS bar_ms,
          FIRST((ask + bid) / 2.0 ORDER BY timestamp_ms)            AS open_mid,
          MAX((ask + bid) / 2.0)                                    AS high_mid,
          MIN((ask + bid) / 2.0)                                    AS low_mid,
          LAST((ask + bid) / 2.0 ORDER BY timestamp_ms)             AS close_mid,
          COUNT(*)                                                  AS tick_n,
          AVG(ask - bid)                                            AS avg_spread,
          MAX(ask - bid)                                            AS max_spread
        FROM read_csv('{src}', header=true,
                      columns={{'timestamp_ms':'BIGINT','ask':'DOUBLE','bid':'DOUBLE',
                                'ask_vol':'DOUBLE','bid_vol':'DOUBLE'}})
        GROUP BY bar_ms
        ORDER BY bar_ms
      )
      TO '{out}' (FORMAT 'parquet', COMPRESSION 'zstd');
    """)
    con.close()
    elapsed = time.time() - t0
    size = os.path.getsize(out)
    print(f"DONE {yyyy_mm}  elapsed={elapsed:.1f}s  size={size/1e6:.1f}MB", flush=True)


def merge_all() -> None:
    parts = sorted(glob.glob(f"{OUT_DIR}/bars_5s_2025_*.parquet")) \
          + sorted(glob.glob(f"{OUT_DIR}/bars_5s_2026_*.parquet"))
    if not parts:
        print("MERGE: no per-month parquets found", flush=True)
        return
    files_sql = ", ".join(f"'{p}'" for p in parts)
    t0 = time.time()
    con = duckdb.connect()
    con.execute(f"""
      COPY (
        SELECT * FROM read_parquet([{files_sql}])
        ORDER BY bar_ms
      )
      TO '{MERGED}' (FORMAT 'parquet', COMPRESSION 'zstd');
    """)
    con.close()
    elapsed = time.time() - t0
    size = os.path.getsize(MERGED)
    print(f"MERGED {len(parts)} months  elapsed={elapsed:.1f}s  size={size/1e6:.1f}MB  out={MERGED}", flush=True)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    if argv[1] == "--merge":
        merge_all()
        return 0
    for arg in argv[1:]:
        aggregate_month(arg)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
