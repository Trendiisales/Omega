#!/usr/bin/env python3
"""Build XAGUSD_2022_2026.h1.csv / .h4.csv (+ .m30 for the histdata span) from
histdata tick zips (2022-01..2024-01, EST stamps -> UTC via +5h) merged with the
existing clean H1 (/Users/jo/Tick/xag/XAGUSD_h1_clean.csv, 2024-01-15..2026-07-06).
Output format: ts,open,high,low,close  (ts = epoch SECONDS of bar start, UTC)."""
import zipfile, glob, io, os, datetime

TICK = "/Users/jo/Tick"
ZIPDIR = f"{TICK}/XAGUSD"
SHIFT = 5 * 3600  # EST -> UTC

def iter_ticks():
    for z in sorted(glob.glob(f"{ZIPDIR}/HISTDATA_COM_ASCII_XAGUSD_T*.zip")):
        zf = zipfile.ZipFile(z)
        name = [n for n in zf.namelist() if n.lower().endswith(".csv")][0]
        with zf.open(name) as f:
            for ln in io.TextIOWrapper(f, "ascii", errors="replace"):
                # YYYYMMDD HHMMSSmmm,bid,ask,vol
                try:
                    dt, rest = ln.split(",", 1)
                    bid_s, ask_s = rest.split(",")[:2]
                    d, t = dt.split(" ")
                    y, mo, dd = int(d[:4]), int(d[4:6]), int(d[6:8])
                    hh, mi, ss = int(t[:2]), int(t[2:4]), int(t[4:6])
                    bid, ask = float(bid_s), float(ask_s)
                except (ValueError, IndexError):
                    continue
                if bid <= 0 or ask <= 0 or ask < bid - 0.5:
                    continue
                ts = int(datetime.datetime(y, mo, dd, hh, mi, ss,
                         tzinfo=datetime.timezone.utc).timestamp()) + SHIFT
                yield ts, (bid + ask) * 0.5, ask - bid

def aggregate(period):
    bars = []
    cur = None
    for ts, mid, spr in TICKS:
        b0 = ts - ts % period
        if cur is None or b0 != cur[0]:
            if cur: bars.append(cur)
            cur = [b0, mid, mid, mid, mid]
        else:
            if mid > cur[2]: cur[2] = mid
            if mid < cur[3]: cur[3] = mid
            cur[4] = mid
    if cur: bars.append(cur)
    return bars

def write(path, bars):
    with open(path, "w") as f:
        f.write("ts,open,high,low,close\n")
        for b in bars:
            f.write(f"{b[0]},{b[1]:.4f},{b[2]:.4f},{b[3]:.4f},{b[4]:.4f}\n")
    print(f"{path}: {len(bars)} bars  {datetime.datetime.utcfromtimestamp(bars[0][0])} -> {datetime.datetime.utcfromtimestamp(bars[-1][0])}")

print("streaming ticks (single pass, cached to lists per period)...")
# one pass, build h1/h4/m30 simultaneously
aggs = {3600: None, 14400: None, 1800: None}
cur = {p: None for p in aggs}
out = {p: [] for p in aggs}
nt = 0; spr_sum = 0.0; spr_n = 0
for ts, mid, spr in iter_ticks():
    nt += 1
    if nt % 50 == 0:
        spr_sum += spr; spr_n += 1
    for p in aggs:
        b0 = ts - ts % p
        c = cur[p]
        if c is None or b0 != c[0]:
            if c: out[p].append(c)
            cur[p] = [b0, mid, mid, mid, mid]
        else:
            if mid > c[2]: c[2] = mid
            if mid < c[3]: c[3] = mid
            c[4] = mid
for p in aggs:
    if cur[p]: out[p].append(cur[p])
print(f"ticks={nt}  median-ish avg spread={spr_sum/max(spr_n,1):.4f}")

# ---- merge with existing clean H1 (starts 2024-01-15) ----
hist_h1 = out[3600]
hist_end = hist_h1[-1][0]
existing = []
with open(f"{TICK}/xag/XAGUSD_h1_clean.csv") as f:
    next(f)
    for ln in f:
        p = ln.strip().split(",")
        if len(p) < 5: continue
        existing.append([int(p[0]), float(p[1]), float(p[2]), float(p[3]), float(p[4])])
# overlap sanity: compare closes on shared hours
ex_map = {b[0]: b[4] for b in existing}
diffs = [abs(b[4] - ex_map[b[0]]) / b[4] for b in hist_h1 if b[0] in ex_map]
if diffs:
    diffs.sort()
    print(f"overlap hours={len(diffs)} median close diff={diffs[len(diffs)//2]*100:.3f}%  p95={diffs[int(len(diffs)*0.95)]*100:.3f}%")
merged = [b for b in hist_h1] + [b for b in existing if b[0] > hist_end]
merged.sort(key=lambda b: b[0])
write(f"{TICK}/XAGUSD_2022_2026.h1.csv", merged)

# H4 from merged H1 (aligned to 4h epoch boundaries)
h4 = []
c = None
for b in merged:
    b0 = b[0] - b[0] % 14400
    if c is None or b0 != c[0]:
        if c: h4.append(c)
        c = [b0, b[1], b[2], b[3], b[4]]
    else:
        c[2] = max(c[2], b[2]); c[3] = min(c[3], b[3]); c[4] = b[4]
if c: h4.append(c)
write(f"{TICK}/XAGUSD_2022_2026.h4.csv", h4)

# m30 for the tick span only (2022-01..2024-01) — honest granularity note
write(f"{TICK}/XAGUSD_2022_2024.m30.csv", out[1800])
