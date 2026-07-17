#!/usr/bin/env python3
"""Splice XAUUSD H1 2022->2026 from certified /Users/jo/Tick sources:
  1. XAU2022_bear_h1.csv          (H1 already, 2022)
  2. xau_h2023_24_m1.csv          (M1 ts,o,h,l,c,spr  2023-01 -> 2024-02)
  3. xau_1m_spliced_2024_2026.csv (M1 ts,o,h,l,c      2024-03 -> 2026-07)
M1 -> H1 by UTC ts//3600. Output: ts,o,h,l,c (seconds). Overlap dedup: later
source wins nothing -- sources are disjoint by construction; assert monotonic.
"""
import sys

OUT = sys.argv[1] if len(sys.argv) > 1 else "/tmp/XAUUSD_2022_2026.h1.csv"

def read_h1(path):
    rows = []
    with open(path) as f:
        for ln in f:
            p = ln.strip().split(',')
            if len(p) < 5 or not p[0].isdigit():
                continue
            rows.append((int(p[0]), float(p[1]), float(p[2]), float(p[3]), float(p[4])))
    return rows

def m1_to_h1(path):
    out = []
    cur = None  # [h1ts, o, h, l, c]
    with open(path) as f:
        for ln in f:
            p = ln.strip().split(',')
            if len(p) < 5 or not p[0].isdigit():
                continue
            ts = int(p[0]); o, h, l, c = (float(x) for x in p[1:5])
            ht = ts - ts % 3600
            if cur is None or ht != cur[0]:
                if cur is not None:
                    out.append(tuple(cur))
                cur = [ht, o, h, l, c]
            else:
                cur[2] = max(cur[2], h); cur[3] = min(cur[3], l); cur[4] = c
    if cur is not None:
        out.append(tuple(cur))
    return out

a = read_h1("/Users/jo/Tick/XAU2022_bear_h1.csv")
b = m1_to_h1("/Users/jo/Tick/xau_h2023_24_m1.csv")
c = m1_to_h1("/Users/jo/Tick/xau_1m_spliced_2024_2026.csv")
print(f"2022 H1: {len(a)}  [{a[0][0]}..{a[-1][0]}]")
print(f"2023-24 M1->H1: {len(b)}  [{b[0][0]}..{b[-1][0]}]")
print(f"2024-26 M1->H1: {len(c)}  [{c[0][0]}..{c[-1][0]}]")
assert a[-1][0] < b[0][0] < c[0][0], "sources overlap"
# b's tail overlaps c's head by a couple of hours (both cover 2024-03-01 boundary):
# keep c (the fresher splice) and trim b.
nb = len(b)
b = [r for r in b if r[0] < c[0][0]]
if len(b) != nb:
    print(f"trimmed {nb - len(b)} overlapping 2023-24 rows at the 2024-03 seam")
allr = a + b + c
prev = 0
for r in allr:
    assert r[0] > prev, f"non-monotonic at {r[0]}"
    prev = r[0]
    assert r[3] <= r[1] <= r[2] and r[3] <= r[4] <= r[2], f"OHLC violation at {r[0]}"
with open(OUT, 'w') as f:
    f.write("ts,o,h,l,c\n")
    for r in allr:
        f.write(f"{r[0]},{r[1]:.4f},{r[2]:.4f},{r[3]:.4f},{r[4]:.4f}\n")
print(f"wrote {len(allr)} H1 bars -> {OUT}")
