#!/usr/bin/env python3
"""
fetch_pump_bars — ONE-TIME bar dump for the engine-faithful exit sweep.

Why: pump_exit_sweep.cpp drives the REAL PumpScalpEngine class over CSV bars
(no Python re-implementation = no drift). It needs the basket's 3m bars on disk.
The IBKR gateway only lives on the VPS, so this runs THERE once and writes a CSV
the C++ sweep then replays offline, repeatably, with no gateway.

Output: backtest/data/pump_bars.csv  (one row per 3m bar, all names/days)
  columns: sym,day,ts_ms,open,high,low,close,volume   (ts_ms = bar START, UTC ms)

Run ON THE VPS (gateway 127.0.0.1:4002), clientId 79:
  python pump/fetch_pump_bars.py
  python pump/fetch_pump_bars.py --days "INHD:20260608 SLGB:20260609 ..."
Then copy backtest/data/pump_bars.csv back to the Mac for the sweep.
"""
import argparse, os, sys, time
from ib_async import IB, Stock

IB_HOST, IB_PORT, IB_CID = "127.0.0.1", 4002, 79
THROTTLE_S = 2.5
TF = "3 mins"          # the engine's only live TF (PumpScalpManager tf_sec=180)

# Same basket pump_exit_bt.py / reentry_cap_bt.py validated on. Extend via --days.
BASKET = ("INHD:20260608 EPSM:20260608 YOUL:20260608 CCTG:20260608 RGNT:20260609 "
          "CHAI:20260609 SLGB:20260609 AZI:20260609 "
          "CIIT:20260610 HCAI:20260610 CHOW:20260610 HKIT:20260610 MSW:20260610 "
          "HWH:20260610 WCT:20260610 VSME:20260610")


def fetch(ib, sym, day):
    c = Stock(sym, "SMART", "USD")
    try:
        q = ib.qualifyContracts(c)
        if q: c = q[0]
    except Exception as e:
        sys.stderr.write(f"[skip] {sym}:{day} qualify failed: {e}\n"); return []
    end = f"{day} 23:59:59 US/Eastern"
    try:
        bars = ib.reqHistoricalData(c, endDateTime=end, durationStr="1 D",
                                    barSizeSetting=TF, whatToShow="TRADES",
                                    useRTH=False, formatDate=1)
    except Exception as e:
        sys.stderr.write(f"[skip] {sym}:{day} hist failed: {e}\n"); return []
    time.sleep(THROTTLE_S)
    rows = []
    for b in bars:
        ts_ms = int(b.date.timestamp() * 1000)
        rows.append((sym, day, ts_ms, b.open, b.high, b.low, b.close, b.volume))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--days", default=BASKET, help='space-sep "SYM:YYYYMMDD" list')
    ap.add_argument("--out", default=None)
    a = ap.parse_args()
    out = a.out or os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                                "backtest", "data", "pump_bars.csv")
    os.makedirs(os.path.dirname(out), exist_ok=True)

    ib = IB(); ib.connect(IB_HOST, IB_PORT, clientId=IB_CID, timeout=15)
    try:
        ib.reqMarketDataType(3)
    except Exception:
        pass

    pairs = [p.split(":") for p in a.days.split()]
    n_bars = 0
    with open(out, "w") as f:
        f.write("sym,day,ts_ms,open,high,low,close,volume\n")
        for sym, day in pairs:
            rows = fetch(ib, sym, day)
            for r in rows:
                f.write(",".join(str(x) for x in r) + "\n")
            n_bars += len(rows)
            print(f"  {sym}:{day}  {len(rows)} bars", flush=True)
    ib.disconnect()
    print(f"\nWROTE {out}  ({len(pairs)} name-days, {n_bars} bars)")


if __name__ == "__main__":
    main()
