#!/usr/bin/env python3
# Pull NQ (CME E-mini Nasdaq-100) 5-minute TRADES bars (OHLCV, REAL CME volume)
# for the Peachy one-candle ORB backtest. ContFuture rejects endDateTime backfill
# (err 10339, see mgc_pull_history.py) -> walk DATED front-month contracts and
# stitch. Each quarterly contract (H/M/U/Z = Mar/Jun/Sep/Dec) is liquid ~3mo
# pre-expiry; we pull its window in 1-month chunks (IBKR 5min duration cap) and
# dedup by timestamp, preferring the most recent contract on overlap.
#
# Run ON THE OMEGA VPS (where the IB gateway lives), port 4002:
#   python tools/nq_pull_history.py [port] [n_contracts]
# -> data/nq_5m_hist.csv   (ts,o,h,l,c,v  -- v = real CME volume)
#
# Then locally backtest with real volume:
#   ./peachy_orb data/nq_5m_hist.csv 15 1330 1600 0.7 2 0.4 2.0 1.0 1  1.0 0.3 1.3 0 NQ
# (the harness auto-detects a 6-col ts,o,h,l,c,v bar file vs a raw tick file --
#  see load path; if not, convert: it already streams 3-col ticks, so for the
#  6-col bar file use the --bars sibling or the bar-loader below.)
import sys, datetime as dt
from ib_async import IB, Future

PORT      = int(sys.argv[1]) if len(sys.argv) > 1 else 4002
NCONTRACT = int(sys.argv[2]) if len(sys.argv) > 2 else 6     # ~ NCONTRACT*3 months back

# quarterly expiry months, 3rd Friday. Build the last NCONTRACT expiries from today.
def quarterly_expiries(n):
    today = dt.date.today()
    months = [3, 6, 9, 12]
    exps = []
    y = today.year + 1
    while len(exps) < n + 2:
        for m in (12, 9, 6, 3):
            # 3rd Friday of month m, year y
            d = dt.date(y, m, 1)
            # weekday(): Mon=0..Sun=6 ; Friday=4
            first_fri = 1 + ((4 - d.weekday()) % 7)
            third_fri = dt.date(y, m, first_fri + 14)
            if third_fri <= today + dt.timedelta(days=20):   # include the active front contract
                exps.append((y, m, third_fri))
        y -= 1
    exps.sort(reverse=True)                                   # most-recent first
    return exps[:n]

ib = IB(); ib.connect("127.0.0.1", PORT, clientId=77, timeout=20)

rows = {}   # ts -> (o,h,l,c,v)
for (y, m, exp) in quarterly_expiries(NCONTRACT):
    ym = f"{y}{m:02d}"
    c = Future("NQ", lastTradeDateOrContractMonth=ym, exchange="CME", currency="USD")
    try:
        ib.qualifyContracts(c)
    except Exception as e:
        print(f"  {ym}: qualify failed: {e}"); continue
    # pull 3 monthly chunks ending at expiry, walking backward
    got = 0
    end = dt.datetime(exp.year, exp.month, exp.day, 21, 0, 0)   # ~ session close UTC-ish
    for _ in range(3):
        endStr = end.strftime("%Y%m%d %H:%M:%S")
        try:
            bars = ib.reqHistoricalData(c, endDateTime=endStr, barSizeSetting="5 mins",
                                        durationStr="1 M", whatToShow="TRADES",
                                        useRTH=False, timeout=300)
        except Exception as e:
            print(f"  {ym} end={endStr} err: {e}"); break
        if not bars:
            break
        for b in bars:
            rows[int(b.date.timestamp())] = (b.open, b.high, b.low, b.close, b.volume or 0)
        got += len(bars)
        end = bars[0].date if not isinstance(bars[0].date, dt.date) or isinstance(bars[0].date, dt.datetime) else dt.datetime.combine(bars[0].date, dt.time())
        end = end - dt.timedelta(seconds=1)
    print(f"  contract NQ {ym} (exp {exp}) -> {got} bars, total {len(rows)}")
ib.disconnect()

import os
os.makedirs("data", exist_ok=True)
with open("data/nq_5m_hist.csv", "w") as f:
    f.write("ts,o,h,l,c,v\n")
    for ts in sorted(rows):
        o, h, l, cl, v = rows[ts]
        f.write(f"{ts},{o},{h},{l},{cl},{v}\n")
print(f"wrote data/nq_5m_hist.csv: {len(rows)} bars w/ real CME volume")
