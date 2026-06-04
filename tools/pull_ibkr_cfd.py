#!/usr/bin/env python3
# pull_ibkr_cfd.py — IBKR Nasdaq-100 CASH CFD intraday, to test whether FVGcont
# (a cash-feed edge) survives on IBKR's cash CFD (so IBKR can host both engines).
# Tries IBKR's index-CFD symbol variants, pulls 15m + 5m RTH. ts,o,h,l,c.
import sys, os, datetime as _dt
from ib_async import IB, Contract
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 4002
os.makedirs("data", exist_ok=True)
def _ts(d): return int(d.timestamp()) if isinstance(d,_dt.datetime) else int(_dt.datetime(d.year,d.month,d.day).timestamp())
ib = IB(); ib.connect("127.0.0.1", PORT, clientId=74, timeout=25)

CANDS = ["IBUS-TECH-100","IBUST100","IBUSTECH100","NDX","IBUS-TECH100","US-TECH 100","USTECH100"]
con=None
for s in CANDS:
    c = Contract(secType="CFD", symbol=s, exchange="SMART", currency="USD")
    try:
        d = ib.reqContractDetails(c)
        if d: con=d[0].contract; print(f"[cfd] qualified CFD '{s}' -> {con.localSymbol} conId={con.conId}", flush=True); break
        else: print(f"[cfd] '{s}': no details", flush=True)
    except Exception as e:
        print(f"[cfd] '{s}': {e}", flush=True)
if not con:
    print("[cfd] NO CFD symbol qualified — IBKR may not offer a NAS100 index CFD here", flush=True)
    ib.disconnect(); sys.exit(0)

def pull(bar,dur,out):
    for w in ("MIDPOINT","TRADES","BID_ASK"):
        try:
            b=ib.reqHistoricalData(con,"",durationStr=dur,barSizeSetting=bar,whatToShow=w,useRTH=True,timeout=180)
            if b:
                with open(out,"w") as f:
                    f.write("ts,open,high,low,close\n")
                    for x in b: f.write(f"{_ts(x.date)},{x.open},{x.high},{x.low},{x.close}\n")
                print(f"[cfd] {out}: {len(b)} bars ({w})", flush=True); return
        except Exception as e: print(f"[cfd] {out} {w}: {e}", flush=True)
pull("15 mins","2 Y","data/IBKRCFD_NAS_15m.csv")
pull("5 mins","1 Y","data/IBKRCFD_NAS_5m.csv")
ib.disconnect()
