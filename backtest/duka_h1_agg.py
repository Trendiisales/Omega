#!/usr/bin/env python3
"""
Aggregate download_dukascopy.py monthly tick CSVs (timestamp_ms,ask,bid,...) into
H1 OHLC bars (ts_sec,o,h,l,c) for the IndexBearShort H1 backtest.

Usage: duka_h1_agg.py <symbol> <out.csv> [div]
  Reads ./duka_ticks/<symbol>_YYYY_MM.csv for all months present.
  mid=(ask+bid)/2; bucket by floor(ts_ms/3600000); price/=div (default 1.0).
"""
import sys, os, glob

def main():
    sym = sys.argv[1]
    out = sys.argv[2]
    div = float(sys.argv[3]) if len(sys.argv) > 3 else 1.0
    files = sorted(glob.glob(f"./duka_ticks/{sym}_*.csv"))
    if not files:
        print(f"NO FILES for {sym} in ./duka_ticks/"); sys.exit(1)
    bars = {}  # hour_bucket -> [o,h,l,c]
    n = 0
    for fp in files:
        with open(fp) as f:
            f.readline()  # header
            for line in f:
                p = line.split(",")
                if len(p) < 3: continue
                try:
                    ts = int(p[0]); ask = float(p[1]); bid = float(p[2])
                except ValueError:
                    continue
                mid = (ask + bid) * 0.5 / div
                if mid <= 0: continue
                b = ts // 3600000
                if b not in bars:
                    bars[b] = [mid, mid, mid, mid]
                else:
                    r = bars[b]
                    if mid > r[1]: r[1] = mid
                    if mid < r[2]: r[2] = mid
                    r[3] = mid
                n += 1
    with open(out, "w") as o:
        o.write("ts,o,h,l,c\n")
        for b in sorted(bars):
            r = bars[b]
            o.write(f"{b*3600},{r[0]:.3f},{r[1]:.3f},{r[2]:.3f},{r[3]:.3f}\n")
    closes = [bars[b][3] for b in sorted(bars)]
    if closes:
        mn, mx = min(closes), max(closes)
        print(f"[H1] {sym}: {len(bars)} bars from {n} ticks -> {out}  close range {mn:.1f}-{mx:.1f}")
    else:
        print(f"[H1] {sym}: 0 bars")

if __name__ == "__main__":
    main()
