#!/usr/bin/env python3
# Calibrate the liquidity gate. $1000 notional. For each (price_min, min_dvol)
# combo, run the engine over all last-month movers and report names-traded +
# net at 2% AND 5% slip. Goal: a gate where net@5% is no longer a disaster
# (we only trade names deep enough that a $1k order fills near the quote).
import csv, sys, importlib.util
spec = importlib.util.spec_from_file_location("prb", "C:/Omega/pump/pump_recalib_bt.py")
prb = importlib.util.module_from_spec(spec); spec.loader.exec_module(prb)
from ib_async import IB

rows = list(csv.DictReader(open("C:/Omega/pump/mover_scan_out.csv")))
names = [(r["symbol"], r["date"].replace("-",""))
         for r in rows if not r["symbol"].endswith(("W","U")) and float(r["open"]) >= 0.30]

ib = IB(); ib.connect("127.0.0.1", 4002, clientId=67, timeout=20)
try: ib.reqMarketDataType(3)
except Exception: pass
data = {}
for sym, ymd in names:
    try:
        b = prb.fetch_3m(ib, sym, ymd)
        if b: data[(sym,ymd)] = b
    except Exception: pass
ib.disconnect()
print(f"# fetched {len(data)} names", file=sys.stderr)

NOTI = 1000
print(f"{'price_min':>9} {'min_dvol$':>10} {'names':>6} {'trades':>7} {'net@1%':>9} {'net@2%':>9} {'net@5%':>9}")
for price_min in (0.0, 1.0, 3.0):
    for min_dvol in (0.0, 100_000.0, 500_000.0, 2_000_000.0):
        traded=set(); nt=0; n1=n2=n5=0.0
        for k,b in data.items():
            r1=prb.run_day(b,100.0,1.0,NOTI,2.0,5,None,None,6.0,False,min_dvol,price_min)
            r2=prb.run_day(b,100.0,2.0,NOTI,2.0,5,None,None,6.0,False,min_dvol,price_min)
            r5=prb.run_day(b,100.0,5.0,NOTI,2.0,5,None,None,6.0,False,min_dvol,price_min)
            if r1: traded.add(k); nt+=len(r1)
            n1+=sum(r1); n2+=sum(r2); n5+=sum(r5)
        print(f"{price_min:9.1f} {min_dvol:10.0f} {len(traded):6d} {nt:7d} {n1:9.0f} {n2:9.0f} {n5:9.0f}")
