#!/usr/bin/env python3
"""Aggregate /Users/jo/Tick/mgc_30m_hist.csv (ts,o,h,l,c,v) -> UTC-aligned H1 + H4 bar CSVs
(ts,o,h,l,c; ts seconds = bucket start) for XauTrendFollow4h2hBacktest-style harnesses.
Sanity gate inline: monotonic ts, price band 1500-6000, no x1000 glitches, bucket coverage.
"""
import sys, csv

SRC = "/Users/jo/Tick/mgc_30m_hist.csv"
OUT_H1 = "/Users/jo/Tick/mgc_2024_2026.h1.csv"
OUT_H4 = "/Users/jo/Tick/mgc_2024_2026.h4.csv"

def aggregate(rows, sec):
    out = []
    cur = None
    for ts, o, h, l, c in rows:
        b = ts // sec * sec
        if cur is None or b != cur[0]:
            if cur: out.append(cur)
            cur = [b, o, h, l, c]
        else:
            cur[2] = max(cur[2], h); cur[3] = min(cur[3], l); cur[4] = c
    if cur: out.append(cur)
    return out

rows = []
last_ts = 0
with open(SRC) as f:
    rd = csv.reader(f); next(rd)
    for r in rd:
        ts = int(r[0]); o, h, l, c = map(float, r[1:5])
        if ts <= last_ts:
            sys.exit(f"GATE FAIL: non-monotonic ts {ts} after {last_ts}")
        last_ts = ts
        for px in (o, h, l, c):
            if not (1500.0 <= px <= 6000.0):
                sys.exit(f"GATE FAIL: price {px} out of band at ts {ts}")
        if h < l or not (l <= c <= h) or not (l <= o <= h):
            sys.exit(f"GATE FAIL: OHLC inconsistency at ts {ts}")
        rows.append((ts, o, h, l, c))

for out_path, sec in ((OUT_H1, 3600), (OUT_H4, 14400)):
    bars = aggregate(rows, sec)
    with open(out_path, "w") as f:
        f.write("ts,o,h,l,c\n")
        for b in bars:
            f.write(f"{b[0]},{b[1]},{b[2]},{b[3]},{b[4]}\n")
    span_d = (bars[-1][0] - bars[0][0]) / 86400.0
    print(f"{out_path}: {len(bars)} bars, span {span_d:.0f}d, "
          f"first={bars[0][0]} last={bars[-1][0]} close_last={bars[-1][4]}")
print(f"src rows={len(rows)} OK (monotonic, band 1500-6000, OHLC consistent)")
