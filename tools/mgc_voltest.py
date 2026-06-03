#!/usr/bin/env python3
# MGC volume-data probe — confirms the COMEX Real-Time sub returns true
# volume-at-price (reqHistogramData) + per-bar volume (reqHistoricalData TRADES)
# for micro gold, before we wire the HTF Candle Volume Profile.
# Uses a distinct client-id so it won't clash with the running DOM bridge.
import sys
from ib_async import IB, ContFuture

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 4002
ib = IB()
try:
    ib.connect("127.0.0.1", PORT, clientId=88, timeout=15)
except Exception as e:
    print(f"CONNECT FAIL on port {PORT}: {e}")
    sys.exit(1)

print(f"connected (server v{ib.client.serverVersion()})")

c = ContFuture("MGC", "COMEX", "USD")
try:
    ib.qualifyContracts(c)
    print(f"MGC resolved: {c.localSymbol} conId={c.conId} exch={c.exchange}")
except Exception as e:
    print(f"QUALIFY FAIL (sub not active / no permission?): {e}")
    ib.disconnect(); sys.exit(1)

# 1) histogram = volume-at-price directly
try:
    h = ib.reqHistogramData(c, useRTH=False, period="2 d")
    if h:
        tot = sum(x.count for x in h)
        top = sorted(h, key=lambda x: -x.count)[:5]
        print(f"HISTOGRAM ok: {len(h)} price bins, total vol={tot:.0f}")
        print("  top volume-at-price (POC candidates):")
        for x in top:
            print(f"    price={x.price:.2f}  vol={x.count:.0f}")
    else:
        print("HISTOGRAM empty (no data returned)")
except Exception as e:
    print(f"HISTOGRAM FAIL: {e}")

# 2) per-bar TRADES bars (OHLCV with real volume) — the per-candle-profile source
try:
    bars = ib.reqHistoricalData(c, "", barSizeSetting="30 mins",
                                durationStr="1 D", whatToShow="TRADES", useRTH=False)
    if bars:
        nz = sum(1 for b in bars if (b.volume or 0) > 0)
        print(f"TRADES bars ok: {len(bars)} bars, {nz} with volume>0")
        for b in bars[-3:]:
            print(f"    {b.date}  c={b.close}  vol={b.volume}")
    else:
        print("TRADES bars empty")
except Exception as e:
    print(f"TRADES FAIL: {e}")

ib.disconnect()
print("done")
