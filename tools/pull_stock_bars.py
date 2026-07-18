#!/usr/bin/env python3
# pull_stock_bars.py — pull US-stock historical bars from IBKR for the
# FvgContinuation stock study. Writes:
#   data/stocks/<SYM>_15m.csv   (ts,o,h,l,c)  RTH 15-min  -> fvg_core harness
#   data/stocks/<SYM>_d1.csv    (ts,o,h,l,c,v) daily       -> stock_screen.py
# Distinct clientId (77) from the DOM bridge (99) and MGC producer (88).
# Run on the VPS (has the IB Gateway connection):
#   python tools/pull_stock_bars.py NVDA,TSLA,AMD,META,AAPL,MSFT,AMZN,GOOGL,AVGO,PLTR
import sys, os, time, datetime as _dt
from ib_async import IB, Stock

def _ts(d):
    if isinstance(d, _dt.datetime): return int(d.timestamp())
    return int(_dt.datetime(d.year, d.month, d.day).timestamp())   # daily bars = date

SYMS = (sys.argv[1] if len(sys.argv) > 1 else
        "NVDA,TSLA,AMD,META,AAPL,MSFT,AMZN,GOOGL,AVGO,PLTR").split(",")
PORT     = int(sys.argv[2]) if len(sys.argv) > 2 else 4001
DUR_15M  = sys.argv[3] if len(sys.argv) > 3 else "2 Y"
OUT      = "data/stocks"
os.makedirs(OUT, exist_ok=True)

ib = IB(); ib.connect("127.0.0.1", PORT, clientId=77, timeout=25)
print(f"[stocks] connected; pulling {len(SYMS)} symbols", flush=True)

def pull(sym, bar, dur, rth, cols_v):
    c = Stock(sym, "SMART", "USD")
    try:
        ib.qualifyContracts(c)
    except Exception as e:
        print(f"[stocks] {sym} qualify FAIL: {e}", flush=True); return None
    try:
        bars = ib.reqHistoricalData(c, "", durationStr=dur, barSizeSetting=bar,
                                    whatToShow="TRADES", useRTH=rth, timeout=120)
    except Exception as e:
        print(f"[stocks] {sym} {bar} reqHist FAIL: {e}", flush=True); return None
    return bars

for sym in SYMS:
    sym = sym.strip()
    b15 = pull(sym, "15 mins", DUR_15M, True, False)
    if b15:
        with open(f"{OUT}/{sym}_15m.csv", "w") as f:
            f.write("ts,open,high,low,close\n")
            for b in b15:
                f.write(f"{_ts(b.date)},{b.open},{b.high},{b.low},{b.close}\n")
        print(f"[stocks] {sym} 15m: {len(b15)} bars", flush=True)
    bd = pull(sym, "1 day", "2 Y", True, True)
    if bd:
        with open(f"{OUT}/{sym}_d1.csv", "w") as f:
            f.write("ts,open,high,low,close,volume\n")
            for b in bd:
                f.write(f"{_ts(b.date)},{b.open},{b.high},{b.low},{b.close},{b.volume or 0}\n")
        print(f"[stocks] {sym} d1: {len(bd)} bars", flush=True)
    time.sleep(1)   # pacing

print("[stocks] done", flush=True)
ib.disconnect()
