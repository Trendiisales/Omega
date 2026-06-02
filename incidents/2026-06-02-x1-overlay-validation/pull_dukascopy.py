#!/usr/bin/env python3
"""
pull_dukascopy.py
=================
Download Dukascopy historical ticks for an instrument over a UTC date range and
write OHLC bars (default M1) plus, optionally, the raw ticks in Omega's tick
format (ts_ms, askPrice, bidPrice).

Format notes
------------
- Dukascopy path month is ZERO-indexed (00 = January), so May -> "04".
- Each hourly .bi5 is LZMA1 ("alone") compressed; records are 20 bytes,
  big-endian >iiiff = (ms_offset, ask_pts, bid_pts, ask_vol, bid_vol).
- Price = points / SCALE. SCALE is instrument-specific:
    XAUUSD            -> 1000     (3 decimals)
    EURUSD/GBPUSD/... -> 100000   (5 decimals, non-JPY FX)
    USDJPY & JPY xs   -> 1000
  Index CFDs use Dukascopy's own symbols (e.g. USA500.IDXUSD, USATECH.IDXUSD,
  DEU.IDXEUR) which differ from broker ".F" names, and their own scale — pass
  --symbol and --scale explicitly for those.

Usage
-----
  python3 pull_dukascopy.py --symbol XAUUSD --from 2026-05-11 --to 2026-06-02 \
      --tf 1 --out XAUUSD_2026-05_m1.csv
"""

import argparse, urllib.request, urllib.error, lzma, struct, sys
import numpy as np, pandas as pd
from datetime import datetime, timezone, timedelta
from concurrent.futures import ThreadPoolExecutor

BASE = "https://datafeed.dukascopy.com/datafeed"


def url_for(symbol, dt):
    # month is zero-indexed in the Dukascopy path
    return f"{BASE}/{symbol}/{dt.year}/{dt.month-1:02d}/{dt.day:02d}/{dt.hour:02d}h_ticks.bi5"


def fetch(symbol, dt):
    req = urllib.request.Request(url_for(symbol, dt), headers={"User-Agent": "Mozilla/5.0"})
    try:
        return dt, urllib.request.urlopen(req, timeout=30).read()
    except urllib.error.HTTPError as e:
        return dt, None if e.code == 404 else None
    except Exception:
        return dt, None


def decode(dt, raw, scale):
    if not raw:
        return None
    try:
        data = lzma.LZMADecompressor(format=lzma.FORMAT_ALONE).decompress(raw)
    except Exception:
        try:
            data = lzma.decompress(raw, format=lzma.FORMAT_AUTO)
        except Exception:
            return None
    n = len(data) // 20
    if n == 0:
        return None
    base = int(dt.timestamp() * 1000)
    ts = np.empty(n, dtype="int64")
    ask = np.empty(n, dtype="float64")
    bid = np.empty(n, dtype="float64")
    for i in range(n):
        ms, ap, bp, av, bv = struct.unpack_from(">iiiff", data, i * 20)
        ts[i] = base + ms
        ask[i] = ap / scale
        bid[i] = bp / scale
    return ts, ask, bid


def main():
    ap = argparse.ArgumentParser(description="Dukascopy tick downloader -> OHLC bars")
    ap.add_argument("--symbol", required=True)
    ap.add_argument("--from", dest="dfrom", required=True, help="UTC YYYY-MM-DD")
    ap.add_argument("--to", dest="dto", required=True, help="UTC YYYY-MM-DD (inclusive)")
    ap.add_argument("--tf", type=int, default=1, help="bar timeframe minutes (default 1)")
    ap.add_argument("--scale", type=float, default=1000.0, help="price = points/scale (XAUUSD=1000)")
    ap.add_argument("--out", default=None, help="bars CSV out (default {symbol}_{from}_m{tf}.csv)")
    ap.add_argument("--ticks-out", default=None, help="also write raw ticks (ts_ms,askPrice,bidPrice)")
    ap.add_argument("--workers", type=int, default=10)
    args = ap.parse_args()

    d0 = datetime.fromisoformat(args.dfrom).replace(tzinfo=timezone.utc)
    d1 = datetime.fromisoformat(args.dto).replace(tzinfo=timezone.utc)
    hours = []
    d = d0
    while d <= d1:
        for h in range(24):
            hours.append(d.replace(hour=h))
        d += timedelta(days=1)

    print(f"Fetching {len(hours)} hours of {args.symbol} ...", file=sys.stderr)
    raws = {}
    with ThreadPoolExecutor(max_workers=args.workers) as ex:
        for dt, raw in ex.map(lambda x: fetch(args.symbol, x), hours):
            raws[dt] = raw
    got = sum(1 for v in raws.values() if v)
    print(f"hours with data: {got}/{len(hours)}", file=sys.stderr)

    tick_parts = []
    for dt in hours:
        dec = decode(dt, raws.get(dt), args.scale)
        if dec is None:
            continue
        ts, ask, bid = dec
        tick_parts.append((ts, ask, bid))
    if not tick_parts:
        raise SystemExit("No ticks decoded — check symbol/scale/date range.")

    ts = np.concatenate([p[0] for p in tick_parts])
    ask = np.concatenate([p[1] for p in tick_parts])
    bid = np.concatenate([p[2] for p in tick_parts])
    order = np.argsort(ts, kind="stable")
    ts, ask, bid = ts[order], ask[order], bid[order]

    if args.ticks_out:
        pd.DataFrame({"": ts, "askPrice": np.round(ask, 5),
                      "bidPrice": np.round(bid, 5)}).to_csv(args.ticks_out, index=False)
        print(f"ticks: {len(ts)} -> {args.ticks_out}")

    mid = (ask + bid) / 2.0
    s = pd.Series(mid, index=pd.to_datetime(ts, unit="ms", utc=True))
    bars = s.resample(f"{args.tf}min").ohlc().dropna(how="any")
    out = args.out or f"{args.symbol}_{args.dfrom}_m{args.tf}.csv"
    pd.DataFrame({"ts_utc": bars.index.strftime("%Y-%m-%dT%H:%M:%SZ"),
                  "open": bars["open"].round(5), "high": bars["high"].round(5),
                  "low": bars["low"].round(5), "close": bars["close"].round(5)}
                 ).to_csv(out, index=False)
    print(f"M{args.tf} bars: {len(bars)}  {bars.index[0]} .. {bars.index[-1]} -> {out}")
    print(f"price range: {bars['low'].min():.3f} - {bars['high'].max():.3f}")


if __name__ == "__main__":
    main()
