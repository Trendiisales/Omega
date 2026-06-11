#!/usr/bin/env python3
# Run the recalibrated pump engine ($5000) over EVERY >100% mover from
# mover_scan_out.csv. Fetch each name's 3m bars on its jump date from IBKR,
# run the deployed config (gate100, trail2, 15min cap, no BE), sum net$ at
# 1% / 2% / 5% slip. Reports how many names the engine actually traded
# (gate needs an intraday +100% continuation, not an overnight gap).
import csv, sys, importlib.util
spec = importlib.util.spec_from_file_location("prb", "C:/Omega/pump/pump_recalib_bt.py")
prb = importlib.util.module_from_spec(spec); spec.loader.exec_module(prb)
from ib_async import IB

rows = list(csv.DictReader(open("C:/Omega/pump/mover_scan_out.csv")))
# filter artifacts: warrants/units (W/U suffix), sub-cent opens (split glitches)
names = [(r["symbol"], r["date"].replace("-",""), float(r["open"]), float(r["best_pct"]))
         for r in rows
         if not r["symbol"].endswith(("W","U")) and float(r["open"]) >= 0.30]
print(f"# {len(names)} tradeable movers (of {len(rows)} raw) after dropping warrants/units/sub-30c", file=sys.stderr)

ib = IB(); ib.connect("127.0.0.1", 4002, clientId=68, timeout=20)
try: ib.reqMarketDataType(3)
except Exception: pass

results = []  # (sym, date, best%, traded, net1, net2, net5)
for i,(sym, ymd, op, bestpct) in enumerate(names):
    try:
        bars = prb.fetch_3m(ib, sym, ymd)
    except Exception:
        bars = []
    if not bars:
        results.append((sym, ymd, bestpct, 0, 0.0, 0.0, 0.0)); continue
    n1 = prb.run_day(bars, 100.0, 1.0, 5000, 2.0, 5, None, None, 6.0, False)
    n2 = prb.run_day(bars, 100.0, 2.0, 5000, 2.0, 5, None, None, 6.0, False)
    n5 = prb.run_day(bars, 100.0, 5.0, 5000, 2.0, 5, None, None, 6.0, False)
    results.append((sym, ymd, bestpct, len(n1), sum(n1), sum(n2), sum(n5)))
    if (i+1) % 25 == 0: print(f"# {i+1}/{len(names)} simmed", file=sys.stderr)
ib.disconnect()

traded = [r for r in results if r[3] > 0]
def tot(idx): return sum(r[idx] for r in results)
print(f"\n=== $5000-per-position over last-month >100% movers ===")
print(f"names scanned       : {len(results)}")
print(f"names engine TRADED  : {len(traded)}  (rest never armed the +100% intraday gate)")
print(f"total trades         : {sum(r[3] for r in results)}")
print(f"NET @ 1% slip        : ${tot(4):,.0f}")
print(f"NET @ 2% slip        : ${tot(5):,.0f}")
print(f"NET @ 5% slip        : ${tot(6):,.0f}")
traded.sort(key=lambda r: r[5], reverse=True)
print(f"\ntop 20 contributors (net @ 2% slip):")
print(f"{'sym':7}{'date':10}{'best%':>7}{'trades':>7}{'net@1%':>9}{'net@2%':>9}{'net@5%':>9}")
for r in traded[:20]:
    print(f"{r[0]:7}{r[1]:10}{r[2]:7.0f}{r[3]:7d}{r[4]:9.0f}{r[5]:9.0f}{r[6]:9.0f}")
