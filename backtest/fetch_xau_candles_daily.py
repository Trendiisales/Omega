#!/usr/bin/env python3
"""Dukascopy daily 1m-candle fetch for XAUUSD.
URL: /datafeed/XAUUSD/{Y}/{M-1:02d}/{D:02d}/{BID|ASK}_candles_min_1.bi5
Record: 24 bytes big-endian: ts_offset_sec(I), open(I), close(I), low(I),
high(I) scaled by 1000 for XAUUSD, volume(f).
Output csv: ts,o,h,l,c  (mid = (bid+ask)/2 per component, ts sec UTC)
Committed tail for the S-2026-07-14ba study: backtest/data/xau_1m_duka_tail_2026.csv
(2026-06-28..2026-07-13; Jul-14 had no published candles at fetch time).
Usage: fetch_xau_candles_daily.py YYYY-MM-DD YYYY-MM-DD out.csv
"""
import sys, datetime, struct, lzma, time, urllib.request, urllib.error, csv

BASE = "https://datafeed.dukascopy.com/datafeed"
UA = "Mozilla/5.0 (compatible; tick-downloader/1.0)"
DIV = 1000.0

def fetch(url):
    for attempt in range(15):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": UA})
            with urllib.request.urlopen(req, timeout=30) as r:
                return r.read(), "ok"
        except urllib.error.HTTPError as e:
            if e.code == 404: return b"", "404"
            time.sleep(2 * (attempt + 1))
        except Exception:
            time.sleep(2 * (attempt + 1))
    return b"", "HOLE"

def candles(raw):
    if not raw: return {}
    dec = lzma.decompress(raw)
    out = {}
    for i in range(len(dec) // 24):
        ts_off, o, c, l, h, vol = struct.unpack_from(">IIIIIf", dec, i * 24)
        if vol == 0.0 and o == c == l == h: continue   # empty minute filler
        out[ts_off] = (o / DIV, h / DIV, l / DIV, c / DIV)
    return out

def main():
    start = datetime.date.fromisoformat(sys.argv[1])
    end = datetime.date.fromisoformat(sys.argv[2])
    out_path = sys.argv[3]
    rows = []
    d = start
    while d < end:
        day0 = int(datetime.datetime(d.year, d.month, d.day,
                   tzinfo=datetime.timezone.utc).timestamp())
        got = {}
        for side in ("BID", "ASK"):
            url = f"{BASE}/XAUUSD/{d.year}/{d.month-1:02d}/{d.day:02d}/{side}_candles_min_1.bi5"
            raw, st = fetch(url)
            if st == "HOLE":
                print(f"HOLE {d} {side}", flush=True); continue
            got[side] = candles(raw)
        b = got.get("BID", {}); a = got.get("ASK", {})
        n = 0
        for ts_off in sorted(set(b) & set(a)):
            bo, bh, bl, bc = b[ts_off]; ao, ah, al, ac = a[ts_off]
            rows.append((day0 + ts_off, (bo+ao)/2, (bh+ah)/2, (bl+al)/2, (bc+ac)/2))
            n += 1
        print(f"{d}: {n} 1m candles", flush=True)
        d += datetime.timedelta(days=1)
    with open(out_path, "w", newline="") as f:
        w = csv.writer(f)
        for ts, o, h, l, c in rows:
            w.writerow([ts, f"{o:.3f}", f"{h:.3f}", f"{l:.3f}", f"{c:.3f}"])
    print(f"DONE {len(rows)} bars -> {out_path}")

if __name__ == "__main__":
    main()
