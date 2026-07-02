#!/usr/bin/env python3
"""Pull full hourly candle history for BTC-USD / ETH-USD from Coinbase Exchange API.
Output: <sym>_1h.csv with ts,open,high,low,close,volume (ts = epoch seconds, UTC, ascending).
Coinbase returns [time, low, high, open, close, volume], max 300 candles/request.
Usage: python3 pull_coinbase.py [outdir]   (~530 requests, ~6 min. Binance is
geo-blocked (451) in some regions; Coinbase public candles need no key.)
"""
import urllib.request, json, time, csv, sys, datetime as dt

OUT = sys.argv[1] if len(sys.argv) > 1 else '/tmp/crypto_bear_bounce'
GRAN = 3600
CHUNK = 300 * GRAN  # seconds per request window

def pull(sym, start_iso):
    start = int(dt.datetime.fromisoformat(start_iso).replace(tzinfo=dt.timezone.utc).timestamp())
    end_now = int(time.time()) // GRAN * GRAN
    rows = {}
    t0 = start
    nreq = 0
    while t0 < end_now:
        t1 = min(t0 + CHUNK, end_now)
        u = (f'https://api.exchange.coinbase.com/products/{sym}/candles'
             f'?granularity={GRAN}'
             f'&start={dt.datetime.fromtimestamp(t0, dt.timezone.utc).isoformat()}'
             f'&end={dt.datetime.fromtimestamp(t1, dt.timezone.utc).isoformat()}')
        for attempt in range(5):
            try:
                req = urllib.request.Request(u, headers={'User-Agent': 'Mozilla/5.0'})
                data = json.loads(urllib.request.urlopen(req, timeout=30).read())
                break
            except Exception as e:
                if attempt == 4:
                    print(f'{sym} GIVE UP at {t0}: {e}', flush=True)
                    data = []
                else:
                    time.sleep(2 * (attempt + 1))
        for c in data:
            ts, lo, hi, op, cl, vol = c
            rows[int(ts)] = (op, hi, lo, cl, vol)
        nreq += 1
        if nreq % 50 == 0:
            print(f'{sym}: {nreq} reqs, {len(rows)} candles, at {dt.datetime.fromtimestamp(t0, dt.timezone.utc).date()}', flush=True)
        t0 = t1
        time.sleep(0.25)
    fp = f'{OUT}/{sym.replace("-","")}_1h.csv'
    with open(fp, 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['ts', 'open', 'high', 'low', 'close', 'volume'])
        for ts in sorted(rows):
            op, hi, lo, cl, vol = rows[ts]
            w.writerow([ts, op, hi, lo, cl, vol])
    print(f'{sym}: DONE {len(rows)} candles -> {fp}', flush=True)

if __name__ == '__main__':
    pull('BTC-USD', '2017-01-01')
    pull('ETH-USD', '2017-01-01')
