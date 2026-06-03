#!/usr/bin/env python3
# Pull ~2yr of MGC 30m TRADES bars (OHLCV, real volume) for the volume-profile
# confluence backtest. Chunks backward in 6-month windows (IBKR 30m limit).
# Run: python tools/mgc_pull_history.py [port]   -> data/mgc_30m_hist.csv
import sys, datetime as dt
from ib_async import IB, ContFuture

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 4002
ib = IB(); ib.connect("127.0.0.1", PORT, clientId=88, timeout=20)
c = ContFuture("MGC", "COMEX", "USD"); ib.qualifyContracts(c)

rows = {}
# ContFuture rejects endDateTime backfill (err 10339) -> single large pull.
for dur in ["2 Y", "1 Y", "6 M"]:
    try:
        bars = ib.reqHistoricalData(c, "", barSizeSetting="30 mins",
                                    durationStr=dur, whatToShow="TRADES",
                                    useRTH=False, timeout=300)
        if bars:
            for b in bars:
                rows[int(b.date.timestamp())] = (b.open, b.high, b.low, b.close, b.volume or 0)
            print(f"  dur={dur} -> {len(bars)} bars, earliest {bars[0].date}")
            break
        print(f"  dur={dur}: empty")
    except Exception as e:
        print(f"  dur={dur} err: {e}")
ib.disconnect()

with open("data/mgc_30m_hist.csv", "w") as f:
    f.write("ts,o,h,l,c,v\n")
    for ts in sorted(rows):
        o, h, l, cl, v = rows[ts]
        f.write(f"{ts},{o},{h},{l},{cl},{v}\n")
print(f"wrote data/mgc_30m_hist.csv: {len(rows)} bars")
