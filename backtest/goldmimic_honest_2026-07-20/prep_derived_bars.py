#!/usr/bin/env python3
# Derive bar files for the gold-mimic honest re-cert (S-2026-07-20 resume).
# Sources are ALL data_integrity_gate CERTIFIED upstream:
#   /Users/jo/Tick/2yr_XAUUSD_tick_fresh.h1.csv  -> xau_d1_from_h1.csv  (UTC day buckets)
#     (replaces /Users/jo/Tick/2yr_XAUUSD_daily.csv which the gate REJECTED:
#      1 backward ts jump of 154h -> not chronological)
#   backtest/data/mgc_30m_spliced_2024_2026.csv  -> mgc_h1_from_m30.csv (to_h1 semantics:
#      bucket keyed by first row seen in the hour -- matches gold_shorttf_bothways_bt.cpp)
#   /Users/jo/Tick/xau_1m_spliced_2024_2026.csv  -> xau_10m_from_1m.csv (resample() semantics)
import sys, os

OUT = os.path.dirname(os.path.abspath(__file__))

def agg(src, dst, step, skip_header):
    out = []
    with open(src) as f:
        if skip_header:
            f.readline()
        for line in f:
            p = line.strip().split(',')
            if len(p) < 5:
                continue
            try:
                ts = int(float(p[0])); o, h, l, c = map(float, p[1:5])
            except ValueError:
                continue
            b = (ts // step) * step
            # to_h1/resample semantics: bucket keyed by comparison with LAST bucket only
            if not out or out[-1][0] != b:
                out.append([b, o, h, l, c])
            else:
                x = out[-1]
                x[2] = max(x[2], h); x[3] = min(x[3], l); x[4] = c
    with open(dst, 'w') as f:
        f.write("ts,o,h,l,c\n")
        for b, o, h, l, c in out:
            f.write(f"{b},{o:.5f},{h:.5f},{l:.5f},{c:.5f}\n")
    print(f"{dst}: {len(out)} bars from {src}")

agg("/Users/jo/Tick/2yr_XAUUSD_tick_fresh.h1.csv", f"{OUT}/xau_d1_from_h1.csv", 86400, True)
agg("/Users/jo/Omega/backtest/data/mgc_30m_spliced_2024_2026.csv", f"{OUT}/mgc_h1_from_m30.csv", 3600, True)
agg("/Users/jo/Tick/xau_1m_spliced_2024_2026.csv", f"{OUT}/xau_10m_from_1m.csv", 600, False)
