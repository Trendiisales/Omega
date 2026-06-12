#!/usr/bin/env python3
"""
mover_scan — find every US stock that jumped >100% intraday in the last month,
using ONLY free sources (Nasdaq Trader symbol lists + Yahoo daily bars, no key).

Universe : nasdaqlisted.txt + otherlisted.txt  (~11k US symbols)
Per name : Yahoo v8 chart range=1mo interval=1d -> for each day compute
             intraday% = (high/open - 1)*100   and   gap%/run = (high/prevClose - 1)*100
           keep the symbol's BEST day if either >= THRESH.
Output   : pump/mover_scan_out40.csv  (symbol,date,open,high,close,intraday_pct,vs_prevclose_pct)
"""
import csv, io, sys, time, urllib.request, concurrent.futures as cf

THRESH = 40.0
UA = {"User-Agent": "Mozilla/5.0"}

def get(url, timeout=20):
    req = urllib.request.Request(url, headers=UA)
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read().decode("utf-8", "replace")

def universe():
    syms = set()
    for url, col in [
        ("https://www.nasdaqtrader.com/dynamic/SymDir/nasdaqlisted.txt", 0),
        ("https://www.nasdaqtrader.com/dynamic/SymDir/otherlisted.txt", 0),
    ]:
        try:
            txt = get(url)
        except Exception as e:
            print(f"# universe fetch fail {url}: {e}", file=sys.stderr); continue
        for ln in txt.splitlines()[1:]:
            if ln.startswith("File Creation"): continue
            parts = ln.split("|")
            if len(parts) <= col: continue
            s = parts[col].strip()
            # skip test issues / warrants / units / non-common where possible
            if not s or any(c in s for c in ".$") or len(s) > 5: continue
            syms.add(s)
    return sorted(syms)

def best_day(sym):
    url = f"https://query1.finance.yahoo.com/v8/finance/chart/{sym}?range=1mo&interval=1d"
    try:
        import json
        j = json.loads(get(url, timeout=15))
        res = j["chart"]["result"][0]
        ts = res["timestamp"]; q = res["indicators"]["quote"][0]
        o, h, c = q["open"], q["high"], q["close"]
    except Exception:
        return None
    best = None; prevc = None
    for i in range(len(ts)):
        oi, hi, ci = o[i], h[i], c[i]
        if oi is None or hi is None or oi <= 0: prevc = ci if ci else prevc; continue
        intraday = (hi/oi - 1)*100
        gap = (hi/prevc - 1)*100 if prevc and prevc > 0 else 0
        score = max(intraday, gap)
        if score >= THRESH and (best is None or score > best[5]):
            import datetime
            d = datetime.datetime.utcfromtimestamp(ts[i]).strftime("%Y-%m-%d")
            best = (sym, d, oi, hi, ci if ci is not None else 0.0, score, intraday, gap)
        prevc = ci if ci else prevc
    return best

def main():
    syms = universe()
    print(f"# universe: {len(syms)} symbols; scanning Yahoo 1mo daily...", file=sys.stderr)
    hits = []
    done = 0
    with cf.ThreadPoolExecutor(max_workers=12) as ex:
        futs = {ex.submit(best_day, s): s for s in syms}
        for fut in cf.as_completed(futs):
            done += 1
            if done % 500 == 0: print(f"# {done}/{len(syms)} scanned, {len(hits)} hits", file=sys.stderr)
            r = fut.result()
            if r: hits.append(r)
    hits.sort(key=lambda x: x[5], reverse=True)
    with open("pump/mover_scan_out40.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["symbol","date","open","high","close","best_pct","intraday_pct","vs_prevclose_pct"])
        for h in hits: w.writerow([h[0],h[1],f"{h[2]:.4f}",f"{h[3]:.4f}",f"{h[4]:.4f}",f"{h[5]:.0f}",f"{h[6]:.0f}",f"{h[7]:.0f}"])
    print(f"\nDONE: {len(hits)} stocks jumped >={THRESH:.0f}% in the last month")
    print(f"{'symbol':8} {'date':11} {'open':>9} {'high':>9} {'best%':>7}")
    for h in hits[:40]:
        print(f"{h[0]:8} {h[1]:11} {h[2]:9.3f} {h[3]:9.2f} {h[5]:7.0f}")

if __name__ == "__main__":
    main()
