#!/usr/bin/env python3
"""pull_ibkr_ticks.py -- backfill historical futures TAPE from IBKR so the Aurora
predicate study has a real sample NOW instead of waiting weeks for the live
bridge to accumulate.

Uses reqHistoricalTicks (free, same CME Real-Time sub the live tape uses) to pull
TRADES (price+size) + BID_ASK (for delta classification) for a futures symbol over
the last N days, paginated 1000 ticks/request walking forward. Writes the SAME
CSV format the bridge produces, partitioned by UTC date:
    ibkr_trades_<SYM>_<DATE>.csv  : ts_ms,price,size,exch,spec
    ibkr_l2_<SYM>_<DATE>.csv      : ts_ms,mid,bid,ask
so aurora_flow.py / aurora_predicate.py read it directly (mixed with live tape).

usage (on the VPS, live gateway up):
  python ibkr/pull_ibkr_ticks.py --symbol NAS100 --days 10 --out-dir C:\\Omega\\logs\\ibkr_l2
  python ibkr/pull_ibkr_ticks.py --symbol MGC    --days 10 --out-dir C:\\Omega\\logs\\ibkr_l2
then:
  python ibkr/aurora_predicate.py --trades "...ibkr_trades_NQ_*.csv" --l2 "...ibkr_l2_NQ_*.csv" --tf-min 5

NOTE: IBKR paces historical-tick requests; a liquid future over many days = many
requests. Start with --days 5-10. ContFuture is used (IBKR auto-rolls); good
enough for a predicate study.
"""
from __future__ import annotations
import argparse, csv, os, sys, time
import datetime as dt
from collections import defaultdict
from ib_async import IB, ContFuture

# symbol -> ContFuture (symbol, exchange, currency). Mirrors the bridge map.
FUT = {
    "MGC":    dict(symbol="MGC", exchange="COMEX", currency="USD"),
    "NAS100": dict(symbol="NQ",  exchange="CME",   currency="USD"),
    "NQ":     dict(symbol="NQ",  exchange="CME",   currency="USD"),
    "US500":  dict(symbol="ES",  exchange="CME",   currency="USD"),
    "ES":     dict(symbol="ES",  exchange="CME",   currency="USD"),
}


def utc_date(ts):
    return dt.datetime.utcfromtimestamp(ts).strftime("%Y-%m-%d")


def pull(ib, contract, start, end, what):
    """Walk forward from `start`, 1000 ticks/request, until `end`. Returns rows."""
    rows = []
    cur = start
    last_ts = 0
    while cur < end:
        ticks = ib.reqHistoricalTicks(contract, cur, "", 1000, what, useRth=False,
                                      ignoreSize=False)
        if not ticks:
            break
        for t in ticks:
            ts = t.time.timestamp() if hasattr(t.time, "timestamp") else 0
            rows.append((ts, t))
        newest = ticks[-1].time
        if newest.timestamp() <= last_ts:
            break  # no progress -> done
        last_ts = newest.timestamp()
        cur = newest + dt.timedelta(seconds=1)
        time.sleep(0.12)  # pacing
        if cur >= end:
            break
    return rows


def write_partitioned(rows, out_dir, sym, what):
    """Write rows partitioned by UTC date in the bridge CSV format."""
    by_date = defaultdict(list)
    for ts, t in rows:
        by_date[utc_date(ts)].append((ts, t))
    os.makedirs(out_dir, exist_ok=True)
    total = 0
    for date_str, items in by_date.items():
        if what == "TRADES":
            path = os.path.join(out_dir, f"ibkr_trades_{sym}_{date_str}.csv")
            new = not os.path.exists(path) or os.path.getsize(path) == 0
            with open(path, "a", newline="") as fh:
                w = csv.writer(fh)
                if new:
                    w.writerow(["ts_ms", "price", "size", "exch", "spec"])
                for ts, t in items:
                    w.writerow([int(ts * 1000), f"{t.price:.4f}", f"{t.size:.2f}",
                                getattr(t, "exchange", ""), getattr(t, "specialConditions", "")])
                    total += 1
        else:  # BID_ASK
            path = os.path.join(out_dir, f"ibkr_l2_{sym}_{date_str}.csv")
            new = not os.path.exists(path) or os.path.getsize(path) == 0
            with open(path, "a", newline="") as fh:
                w = csv.writer(fh)
                if new:
                    w.writerow(["ts_ms", "mid", "bid", "ask"])
                for ts, t in items:
                    b, a = t.priceBid, t.priceAsk
                    if b and a and b > 0 and a > 0:
                        w.writerow([int(ts * 1000), f"{(b+a)/2:.4f}", f"{b:.4f}", f"{a:.4f}"])
                        total += 1
    return total


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--symbol", required=True, choices=list(FUT))
    ap.add_argument("--days", type=int, default=10)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=4001)
    ap.add_argument("--client-id", type=int, default=81)
    ap.add_argument("--out-dir", default=r"C:\Omega\logs\ibkr_l2")
    a = ap.parse_args()

    ib = IB()
    ib.connect(a.host, a.port, clientId=a.client_id, timeout=20)
    ib.reqMarketDataType(1)
    m = FUT[a.symbol]
    c = ContFuture(symbol=m["symbol"], exchange=m["exchange"], currency=m["currency"])
    ib.qualifyContracts(c)
    fname_sym = m["symbol"]   # 'NQ'/'MGC'/'ES' -> matches aurora glob
    print(f"[pull] {a.symbol} -> {c.localSymbol or m['symbol']} ({m['exchange']}), {a.days}d", flush=True)

    end = dt.datetime.now(dt.timezone.utc).replace(tzinfo=None)
    start = end - dt.timedelta(days=a.days)
    for what in ("TRADES", "BID_ASK"):
        print(f"[pull] {what} ...", flush=True)
        rows = pull(ib, c, start, end, what)
        n = write_partitioned(rows, a.out_dir, fname_sym, what)
        print(f"[pull] {what}: {len(rows)} ticks -> {n} rows written", flush=True)
    ib.disconnect()
    print(f"[pull] DONE. Run aurora_predicate on ibkr_*_{fname_sym}_*.csv", flush=True)


if __name__ == "__main__":
    main()
