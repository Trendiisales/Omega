#!/usr/bin/env python3
"""
Fetch SPLIT-ADJUSTED daily OHLC for the 39 BIGCAP names (Yahoo chart, interval=1d).

Why: the base mimic ladder (bigcap_upjump_ladder_bt.py) runs on CLOSES ONLY
(data/rdagent/sp500_long_close.csv). To test the intraday + EOD-flat + reversal
exit we need at least the daily OPEN (=> overnight gap = prev_close->open) and the
intraday HIGH/LOW (=> the intra-session adverse extreme a mid-session cut could
fire on). Daily OHLC is the FINEST multi-year granularity that exists for these 39
names anywhere (no stored 5m/H1 equity history in /Tick or the repo — only Yahoo
5m/60d live or the IBKR live gateway). This is the honest data floor; a true
sub-daily proxy built from these bars is labelled a PROXY in the harness.

Adjustment: Yahoo `quote` O/H/L/C is RAW (unadjusted); `adjclose` is split+div
adjusted. We scale O/H/L/C by (adjclose/close) per day => fully adjusted OHLC with
NO split seams (a 10:1 split would otherwise read as a fake -90% overnight gap and
manufacture a bogus tail). This matches the adjusted-close spine the base book uses.

Output: backtest/data/bigcap_daily_ohlc/<SYM>.csv  columns: YYYYMMDD,o,h,l,c
Also a combined wide close file is NOT written — the reversal harness reads per-name.
"""
import urllib.request, json, time, os, sys, datetime

BIGCAP = ("NVDA AMD AVGO MU MRVL SMCI ARM PLTR TSLA META NFLX CRWD SHOP COIN MSTR "
          "SNOW NOW PANW UBER ABNB DELL ORCL QCOM INTC AMZN GOOGL MSFT AAPL CRM ADBE "
          "IONQ RGTI QBTS ASTS RKLB NBIS CRWV ALAB CRDO "
          "WDC STX DD TPR BMY SWKS").split()   # S-2026-07-17j: 39->45, match BIGCAP_LAD
UA = {"User-Agent": "Mozilla/5.0"}
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data", "bigcap_daily_ohlc")


def get(u, t=25):
    return urllib.request.urlopen(urllib.request.Request(u, headers=UA), timeout=t).read().decode("utf-8", "replace")


def fetch(sym):
    url = f"https://query1.finance.yahoo.com/v8/finance/chart/{sym}?range=10y&interval=1d&events=split"
    j = json.loads(get(url))
    r = j["chart"]["result"][0]
    ts = r["timestamp"]
    q = r["indicators"]["quote"][0]
    adj = r["indicators"].get("adjclose", [{}])[0].get("adjclose")
    o, h, l, c = q["open"], q["high"], q["low"], q["close"]
    rows = []
    for i in range(len(ts)):
        if None in (o[i], h[i], l[i], c[i]) or adj is None or adj[i] is None or c[i] == 0:
            continue
        k = adj[i] / c[i]                       # split/div adjustment factor for this day
        d = datetime.datetime.utcfromtimestamp(ts[i]).strftime("%Y%m%d")
        rows.append((d, o[i] * k, h[i] * k, l[i] * k, c[i] * k))
    return rows


def main():
    os.makedirs(OUT, exist_ok=True)
    tot = 0
    for s in BIGCAP:
        try:
            rows = fetch(s)
        except Exception as e:
            sys.stderr.write(f"[FAIL] {s}: {type(e).__name__} {e}\n")
            time.sleep(1.0)
            continue
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
