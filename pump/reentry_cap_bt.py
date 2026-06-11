#!/usr/bin/env python3
# Backtest the per-name RE-ENTRY CAP on the deployed 3m pump config.
# Live shadow showed CHOW entered 4x (+50,-32,-38,-39 = re-entry chop bleed).
# Compare max_entries = unlimited(0) vs 1 vs 2 over the recalibration basket,
# deployed config (gate100, trail2, cap5/15min, no-BE, hard6, $1000, liq gate
# p>=1 & $vol>=2M), both 1%/2% slip. Run ON THE VPS (gateway 127.0.0.1:4002).
import sys, importlib.util
spec = importlib.util.spec_from_file_location("prb", "C:/Omega/pump/pump_recalib_bt.py")
prb = importlib.util.module_from_spec(spec); spec.loader.exec_module(prb)
from ib_async import IB

NOTI, TRAIL, CAP, HARD = 1000, 2.0, 5, 6.0
MIN_DVOL, PRICE_MIN = 2.0e6, 1.0    # deployed liquidity gate

ib = IB(); ib.connect("127.0.0.1", 4002, clientId=78, timeout=20)
try: ib.reqMarketDataType(3)
except Exception: pass
data = {}
for tok in prb.BASKET.split():
    sym, day = tok.split(":")
    try:
        b = prb.fetch_3m(ib, sym, day)
        if b: data[tok] = b
    except Exception as e:
        print(f"# {tok} fetch fail {e}", file=sys.stderr)
ib.disconnect()
print(f"# fetched {len(data)} names", file=sys.stderr)

def run(cap, slip):
    allt = []; per = {}
    for tok, bars in data.items():
        r = prb.run_day(bars, 100.0, slip, NOTI, TRAIL, CAP, None, None, HARD, False,
                        MIN_DVOL, PRICE_MIN, cap)
        allt += r; per[tok] = (len(r), sum(r))
    return allt, per

print(f"{'cap':>4} {'slip':>5} {'trades':>7} {'net$':>9} {'PF':>5} {'win%':>5}")
results = {}
for cap in (0, 1, 2):
    for slip in (1.0, 2.0):
        allt, per = run(cap, slip)
        n, net, pf, wr = prb.stat(allt)
        results[(cap, slip)] = (n, net, per)
        print(f"{cap:>4} {slip:>5.0f} {n:7d} {net:9.0f} {pf:5.2f} {wr:5.0f}")

# show the re-entry-chop names: where uncapped took >1 trade
print("\nnames with >1 entry uncapped (the leak), net @2% slip uncapped -> cap1:")
_, per0 = results[(0, 2.0)]; _, per1 = results[(1, 2.0)]
for tok in sorted(per0, key=lambda t: per0[t][1]):
    n0, net0 = per0[tok]; n1, net1 = per1[tok]
    if n0 > 1:
        print(f"  {tok:16} uncapped n={n0} net={net0:8.0f}  ->  cap1 n={n1} net={net1:8.0f}")
