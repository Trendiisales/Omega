#!/usr/bin/env python3
"""Fetch SPLIT-ADJUSTED daily OHLC for the DIP+TURTLE union + SPY/QQQ market gate.
Reuses the exact adjustment logic of backtest/fetch_bigcap_daily_ohlc.py
(O/H/L/C * adjclose/close => no split seams). Output: backtest/data/bigcap_daily_ohlc/<SYM>.csv
columns: date,o,h,l,c   (YYYYMMDD)."""
import urllib.request, json, time, os, sys, datetime

# DIP family + TURTLE family union + broad-market gate proxies
DIP    = "MU NVDA AVGO DELL CRDO STX INTC AMD AAPL TPR MSFT".split()
TURTLE = "NVDA AVGO STX DD AMD AAPL TPR BMY SWKS MSFT QCOM".split()
GATE   = "SPY QQQ".split()
NAMES  = sorted(set(DIP) | set(TURTLE) | set(GATE))

UA  = {"User-Agent": "Mozilla/5.0"}
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "backtest", "data", "bigcap_daily_ohlc")

def get(u, t=25):
    return urllib.request.urlopen(urllib.request.Request(u, headers=UA), timeout=t).read().decode("utf-8", "replace")

def fetch(sym):
    url = f"https://query1.finance.yahoo.com/v8/finance/chart/{sym}?range=10y&interval=1d&events=split"
    j = json.loads(get(url)); r = j["chart"]["result"][0]
    ts = r["timestamp"]; q = r["indicators"]["quote"][0]
    adj = r["indicators"].get("adjclose", [{}])[0].get("adjclose")
    o, h, l, c = q["open"], q["high"], q["low"], q["close"]
    rows = []
    for i in range(len(ts)):
        if None in (o[i], h[i], l[i], c[i]) or adj is None or adj[i] is None or c[i] == 0:
            continue
        k = adj[i] / c[i]
        d = datetime.datetime.utcfromtimestamp(ts[i]).strftime("%Y%m%d")
        rows.append((d, o[i]*k, h[i]*k, l[i]*k, c[i]*k))
    return rows

def main():
    os.makedirs(OUT, exist_ok=True); tot = 0
    print(f"fetching {len(NAMES)} names: {' '.join(NAMES)}")
    for s in NAMES:
        try:
            rows = fetch(s)
        except Exception as e:
            sys.stderr.write(f"[FAIL] {s}: {type(e).__name__} {e}\n"); time.sleep(1.0); continue
        with open(os.path.join(OUT, f"{s}.csv"), "w") as f:
            f.write("date,o,h,l,c\n")
            for d, o, h, l, c in rows:
                f.write(f"{d},{o:.6f},{h:.6f},{l:.6f},{c:.6f}\n")
        tot += len(rows)
        print(f"  {s:6s} {len(rows):5d} bars  {rows[0][0]}..{rows[-1][0]}")
        time.sleep(0.4)
    print(f"total {tot} bars -> {OUT}")

if __name__ == "__main__":
    main()
