#!/usr/bin/env python3
# pull_nas_cash.py — NAS-100 CASH index (NDX) intraday from IBKR + a cash-vs-
# future price snapshot. The continuation edge tested strong on the cash feed
# (NSXUSD) but weak on the NQ future; retail index CFDs (what Omega trades) are
# cash-type, so the cash backtest is the relevant one. This pulls the cash NDX
# to confirm on the matching feed. Writes data/NDX_15m.csv, NDX_5m.csv (ts,o,h,l,c).
import sys, os, datetime as _dt
from ib_async import IB, Index, ContFuture
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 4002
os.makedirs("data", exist_ok=True)
def _ts(d): return int(d.timestamp()) if isinstance(d,_dt.datetime) else int(_dt.datetime(d.year,d.month,d.day).timestamp())
ib = IB(); ib.connect("127.0.0.1", PORT, clientId=75, timeout=25)

ndx = Index("NDX", "NASDAQ", "USD"); ib.qualifyContracts(ndx)
nq  = ContFuture("NQ", "CME", "USD"); ib.qualifyContracts(nq)

def pull(con, bar, dur, out):
    for w in ("TRADES", "MIDPOINT"):
        try:
            b = ib.reqHistoricalData(con, "", durationStr=dur, barSizeSetting=bar,
                                     whatToShow=w, useRTH=True, timeout=180)
            if b:
                with open(out, "w") as f:
                    f.write("ts,open,high,low,close\n")
                    for x in b: f.write(f"{_ts(x.date)},{x.open},{x.high},{x.low},{x.close}\n")
                print(f"[nascash] {out}: {len(b)} bars ({w})", flush=True); return
        except Exception as e:
            print(f"[nascash] {out} {w} fail: {e}", flush=True)

pull(ndx, "15 mins", "2 Y", "data/NDX_15m.csv")
pull(ndx, "5 mins",  "1 Y", "data/NDX_5m.csv")

# price snapshot: cash NDX vs NQ future (basis tells us which feed BlackBull tracks)
for con,nm in ((ndx,"NDX cash"),(nq,"NQ future")):
    t = ib.reqMktData(con, "", True, False); ib.sleep(2.5)
    px = t.last or t.close or t.marketPrice()
    print(f"[snapshot] {nm}: {px}", flush=True)
ib.disconnect()
