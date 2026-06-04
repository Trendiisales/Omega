#!/usr/bin/env python3
# pull_index_history.py — deep NQ-future 15m history from IBKR for the bear-OOS
# test (2022 tech bear). Chunks endDateTime backward (IBKR caps intraday hist
# per request), stitches, dedups, writes data/NQ_15m_hist.csv (ts,o,h,l,c) RTH.
#   python tools/pull_index_history.py [end1,end2,...] [port]
# default ends pull 2022+2023+2024 calendar years (1Y chunks).
import sys, os, datetime as _dt
from ib_async import IB, ContFuture

# ContFuture rejects endDateTime (Error 10339) -> single request, no end,
# long duration. IBKR returns as far back as available for the bar size.
DUR  = sys.argv[1] if len(sys.argv) > 1 else "4 Y"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 4002
OUT  = "data/NQ_15m_hist.csv"
os.makedirs("data", exist_ok=True)

def _ts(d):
    return int(d.timestamp()) if isinstance(d, _dt.datetime) else int(_dt.datetime(d.year,d.month,d.day).timestamp())

ib = IB(); ib.connect("127.0.0.1", PORT, clientId=76, timeout=25)
c = ContFuture("NQ", "CME", "USD"); ib.qualifyContracts(c)
print(f"[idxhist] qualified {c.localSymbol}", flush=True)

rows = {}
try:
    bars = ib.reqHistoricalData(c, "", durationStr=DUR,
                                barSizeSetting="15 mins", whatToShow="TRADES",
                                useRTH=True, timeout=300)
    for b in bars:
        rows[_ts(b.date)] = (b.open, b.high, b.low, b.close)
    print(f"[idxhist] dur={DUR}: {len(bars)} bars", flush=True)
except Exception as e:
    print(f"[idxhist] FAIL: {e}", flush=True)

with open(OUT, "w") as f:
    f.write("ts,open,high,low,close\n")
    for ts in sorted(rows):
        o,h,l,cl = rows[ts]; f.write(f"{ts},{o},{h},{l},{cl}\n")
print(f"[idxhist] wrote {OUT}: {len(rows)} bars", flush=True)
ib.disconnect()
