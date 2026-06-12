#!/usr/bin/env python3
"""
largecap_mover_scan — find LIQUID, non-penny stocks that had a big single-day move,
to test the operator's hypothesis: the pump-scalp edge died on micro-cap SLIPPAGE,
not on the momentum. A large-cap that jumps 10-20% (earnings / M&A / news) is a
"major mover" with deep liquidity -> the tight-trail scalp may survive there.

Universe : nasdaqlisted.txt + otherlisted.txt (~11k US symbols), same as mover_scan.
Per name : Yahoo v8 chart range=1y interval=1d -> OHLCV. For each day compute
             intraday% = (high/open - 1)*100
             gap%      = (open/prevClose - 1)*100   (overnight earnings/news gap)
             move%     = max(intraday, gap)
           Keep the day if move% >= MIN_MOVE AND close >= MIN_PRICE AND
           close*volume >= MIN_DVOL_USD  (the liquidity / not-penny filter).
Output   : pump/largecap_movers_out.csv (symbol,date,open,high,close,vol,dvol_usd,
             intraday_pct,gap_pct,move_pct)  + a threshold-bucket frequency summary.
"""
import csv, sys, json, urllib.request, datetime, concurrent.futures as cf

MIN_PRICE    = 10.0          # not a penny stock
MIN_DVOL_USD = 100e6         # >= $100M traded that day = liquid (large/mid-cap)
MIN_MOVE     = 5.0           # capture from 8% up; we bucket higher thresholds in the summary
RANGE        = "1y"
UA = {"User-Agent": "Mozilla/5.0"}

def get(url, timeout=20):
    req = urllib.request.Request(url, headers=UA)
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read().decode("utf-8", "replace")

def universe():
    syms = set()
    for url in ("https://www.nasdaqtrader.com/dynamic/SymDir/nasdaqlisted.txt",
                "https://www.nasdaqtrader.com/dynamic/SymDir/otherlisted.txt"):
        try: txt = get(url)
        except Exception as e:
            print(f"# universe fail {url}: {e}", file=sys.stderr); continue
        for ln in txt.splitlines()[1:]:
            if ln.startswith("File Creation"): continue
            p = ln.split("|")
            if not p: continue
            s = p[0].strip()
            if not s or any(c in s for c in ".$") or len(s) > 5: continue
            syms.add(s)
    return sorted(syms)

def scan(sym):
    url = f"https://query1.finance.yahoo.com/v8/finance/chart/{sym}?range={RANGE}&interval=1d"
    try:
        j = json.loads(get(url, timeout=15))
        res = j["chart"]["result"][0]
        ts = res["timestamp"]; q = res["indicators"]["quote"][0]
        o,h,c,v = q["open"],q["high"],q["close"],q["volume"]
    except Exception:
        return []
    out=[]; prevc=None
    for i in range(len(ts)):
        oi,hi,ci,vi = o[i],h[i],c[i],v[i]
        if oi is None or hi is None or ci is None or vi is None or oi<=0:
            prevc = ci if ci else prevc; continue
        dvol = ci*vi
        intraday = (hi/oi - 1)*100
        gap = (oi/prevc - 1)*100 if prevc and prevc>0 else 0.0
        move = max(intraday, gap)
        if move>=MIN_MOVE and ci>=MIN_PRICE and dvol>=MIN_DVOL_USD:
            d = datetime.datetime.utcfromtimestamp(ts[i]).strftime("%Y-%m-%d")
            out.append((sym,d,oi,hi,ci,vi,dvol,intraday,gap,move))
        prevc = ci if ci else prevc
    return out

def main():
    syms = universe()
    print(f"# universe {len(syms)} syms; scanning Yahoo {RANGE} daily (price>={MIN_PRICE}, $vol>={MIN_DVOL_USD/1e6:.0f}M, move>={MIN_MOVE}%)...", file=sys.stderr)
    hits=[]; done=0
    with cf.ThreadPoolExecutor(max_workers=12) as ex:
        futs={ex.submit(scan,s):s for s in syms}
        for fut in cf.as_completed(futs):
            done+=1
            if done%1000==0: print(f"# {done}/{len(syms)} scanned, {len(hits)} events", file=sys.stderr)
            hits += fut.result()
    hits.sort(key=lambda x:x[9], reverse=True)
    with open("pump/largecap_movers_out.csv","w",newline="") as f:
        w=csv.writer(f); w.writerow(["symbol","date","open","high","close","volume","dvol_usd","intraday_pct","gap_pct","move_pct"])
        for x in hits:
            w.writerow([x[0],x[1],f"{x[2]:.2f}",f"{x[3]:.2f}",f"{x[4]:.2f}",int(x[5]),f"{x[6]:.0f}",f"{x[7]:.1f}",f"{x[8]:.1f}",f"{x[9]:.1f}"])
    # frequency summary by threshold + distinct symbols
    print(f"\nDONE: {len(hits)} liquid big-move events ({RANGE}, ~{len(syms)} syms)")
    for thr in (5,8,10,12,15,20,30,50):
        sub=[x for x in hits if x[9]>=thr]
        nsym=len(set(x[0] for x in sub))
        # split intraday-driven vs gap-driven
        ig=sum(1 for x in sub if x[7]>=x[8]); gp=len(sub)-ig
        print(f"  move>={thr:3}% : {len(sub):4} events / {nsym:3} distinct syms  (intraday-led {ig}, gap-led {gp})")
    print("\ntop 25 by move%:")
    print(f"{'sym':6}{'date':12}{'close':>8}{'$vol(M)':>9}{'intra%':>7}{'gap%':>7}")
    for x in hits[:25]:
        print(f"{x[0]:6}{x[1]:12}{x[4]:8.2f}{x[6]/1e6:9.0f}{x[7]:7.1f}{x[8]:7.1f}")

if __name__=="__main__":
    main()
