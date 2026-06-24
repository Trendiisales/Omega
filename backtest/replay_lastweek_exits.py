#!/usr/bin/env python3
# (S-2026-06-24) Replay last week's ACTUAL BigCapMomo trades (from the live shadow
# ledger) through alternative exits, on the REAL traded names' 5m bars (Yahoo, free,
# within 30d). Answers "how would the new exit have fared on the trades we already made".
# The live exit gave back $841 of $758 peak (net -$82). Test close-based give-back +
# profit-scaled trail (the tick-based GIVEBACK fired on noise; CLOSE-based is the fix).
import urllib.request, json, csv, datetime

LED="/tmp/closes.csv"
def fetch(sym, cache={}):
    if sym in cache: return cache[sym]
    try:
        u=f"https://query1.finance.yahoo.com/v8/finance/chart/{sym}?interval=5m&range=1mo"
        r=urllib.request.Request(u,headers={"User-Agent":"Mozilla/5.0"})
        j=json.load(urllib.request.urlopen(r,timeout=20))
        res=j["chart"]["result"][0]; ts=res["timestamp"]; q=res["indicators"]["quote"][0]
        bars=[]
        for i in range(len(ts)):
            o,h,l,c=q["open"][i],q["high"][i],q["low"][i],q["close"][i]
            if None in (o,h,l,c): continue
            bars.append((ts[i],o,h,l,c))
        cache[sym]=bars; return bars
    except Exception as e:
        cache[sym]=[]; return []

def atr_at(bars, idx, n=14):
    if idx<n: return (bars[idx][2]-bars[idx][3])
    s=0
    for k in range(idx-n+1,idx+1):
        h,l,pc=bars[k][2],bars[k][3],bars[k-1][4]
        s+=max(h-l,abs(h-pc),abs(l-pc))
    return s/n

def replay(bars, e_ts, entry, size, method, **kw):
    # find first bar at/after entry
    si=None
    for i,b in enumerate(bars):
        if b[0]>=e_ts: si=i; break
    if si is None or si>=len(bars)-1: return None
    entry_px=entry; peak=entry; held=0
    MAXBARS=kw.get("maxbars",96)
    for i in range(si+1, min(si+1+MAXBARS, len(bars))):
        ts,o,h,l,c=bars[i]; held+=1
        peak=max(peak,h)
        atr=atr_at(bars,i)
        gain=peak-entry_px
        ex=None
        if method=="actual_trail":            # wide ATR trail, intrabar (peak-5*ATR), low can hit
            stop=peak-5.0*atr
            if l<=stop: ex=min(c,stop)
        elif method=="give_close":             # CLOSE retraces frac of peak gain -> exit on close
            frac=kw["frac"]
            if gain>0 and (peak-c)>=frac*gain: ex=c
        elif method=="pscale_close":           # profit-scaled trail, CLOSE-based
            g_pct=gain/entry_px*100
            f=min(1,max(0,g_pct/kw.get("full",15)))
            mult=5.0+(kw.get("tight",2.0)-5.0)*f
            if c<=peak-mult*atr: ex=c
        elif method=="tighten_pop":            # ride wide until +P%, then tight 2*ATR on close
            g_pct=gain/entry_px*100
            mult=2.0 if g_pct>=kw.get("pop",8) else 5.0
            if c<=peak-mult*atr: ex=c
        if ex is not None:
            return (ex-entry_px)*size
    # ran out -> exit at last close (time)
    return (bars[min(si+MAXBARS,len(bars)-1)][4]-entry_px)*size

def main():
    rows=[r for r in csv.DictReader(open(LED)) if 'BigCap' in r['engine']]
    methods=[("actual_trail",{}),("give_close@0.4",{"m":"give_close","frac":0.4}),
             ("give_close@0.33",{"m":"give_close","frac":0.33}),
             ("pscale6->2@15",{"m":"pscale_close","tight":2.0,"full":15}),
             ("tighten@+8%",{"m":"tighten_pop","pop":8})]
    agg={n:0.0 for n,_ in methods}; nmatch=0; actual_total=0
    for r in rows:
        sym=r['symbol']; e_ts=int(r['entry_ts_unix']); entry=float(r['entry_px']); size=float(r['size'])
        actual_total+=float(r['net_pnl'])
        bars=fetch(sym)
        if not bars: continue
        ok=False
        for name,kw in methods:
            meth=kw.get("m","actual_trail"); kw2={k:v for k,v in kw.items() if k!="m"}
            pnl=replay(bars,e_ts,entry,size,meth,**kw2)
            if pnl is not None: agg[name]+=pnl; ok=True
        if ok: nmatch+=1
    print(f"replayed {nmatch}/{len(rows)} BigCapMomo trades on real 5m bars (Yahoo)")
    print(f"  ACTUAL live net (ledger):        ${actual_total:+.0f}")
    print(f"  replay 'actual_trail' (sanity):  ${agg['actual_trail']:+.0f}   (should ~ track ledger)")
    print("  --- alternative exits (close-based) ---")
    for n,_ in methods:
        if n=="actual_trail": continue
        print(f"  {n:<22} ${agg[n]:+.0f}")
if __name__=="__main__": main()
