#!/usr/bin/env python3
# Live MGC 30m TRADES-bar producer for MgcFastDonchian30m. Loops: pulls recent
# 30m TRADES bars (real OHLCV+volume, same source as the backtest), appends any
# NEW CLOSED bars to data/mgc_30m_live.csv, and refreshes data/mgc_hvn.json
# (prior-day POC+HVN). The Omega engine polls these two files. Distinct
# client-id (88) from the DOM bridge (99).
# Run as a scheduled/loop service:  python tools/mgc_live_bars.py [port] [period_s]
import sys, json, time, os
from collections import defaultdict
from ib_async import IB, ContFuture

PORT   = int(sys.argv[1]) if len(sys.argv) > 1 else 4002
PERIOD = int(sys.argv[2]) if len(sys.argv) > 2 else 300
CSV    = "data/mgc_30m_live.csv"
HVN    = "data/mgc_hvn.json"
BINS   = 30

def last_ts():
    if not os.path.exists(CSV): return 0
    try:
        with open(CSV) as f: rows = f.read().splitlines()[1:]
        return int(rows[-1].split(",")[0]) if rows else 0
    except Exception: return 0

def write_hvn(bars):
    # prior COMPLETED UTC day profile
    byday = defaultdict(list)
    for b in bars: byday[int(b.date.timestamp()) // 86400].append(b)
    days = sorted(byday)
    if len(days) < 2: return
    g = byday[days[-2]]
    hi = max(x.high for x in g); lo = min(x.low for x in g)
    if hi <= lo: return
    bs = (hi - lo) / BINS; vb = [0.0]*BINS
    for x in g:
        i = min(BINS-1, max(0, int((x.close - lo)/bs))); vb[i] += (x.volume or 0)
    mx = max(vb) or 1; pi = vb.index(mx)
    poc = lo + bs*(pi+0.5)
    hvn = [round(lo+bs*(i+0.5),2) for i in range(BINS) if vb[i] >= 0.60*mx]
    json.dump({"poc": round(poc,2), "hvn": hvn, "basis": 0.0,
               "day": str(days[-2])}, open(HVN, "w"))

ib = IB(); ib.connect("127.0.0.1", PORT, clientId=88, timeout=20)
c = ContFuture("MGC", "COMEX", "USD"); ib.qualifyContracts(c)
if not os.path.exists(CSV):
    open(CSV, "w").write("ts,o,h,l,c,v\n")
print(f"[mgc-live] producing {CSV} + {HVN} every {PERIOD}s", flush=True)
while True:
    try:
        bars = ib.reqHistoricalData(c, "", barSizeSetting="30 mins",
                                    durationStr="2 D", whatToShow="TRADES",
                                    useRTH=False, timeout=60)
        if bars:
            lt = last_ts(); appended = 0
            with open(CSV, "a") as f:
                for b in bars[:-1]:               # exclude the still-forming bar
                    ts = int(b.date.timestamp())
                    if ts > lt:
                        f.write(f"{ts},{b.open},{b.high},{b.low},{b.close},{b.volume or 0}\n"); appended += 1
            write_hvn(bars)
            if appended: print(f"[mgc-live] +{appended} closed bars, last close {bars[-2].close}", flush=True)
    except Exception as e:
        print(f"[mgc-live] err: {e}", flush=True)
    ib.sleep(PERIOD)
