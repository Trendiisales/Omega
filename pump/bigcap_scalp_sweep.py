#!/usr/bin/env python3
"""
bigcap_scalp_sweep — pull 5m big-cap bars ONCE (cache), then sweep the REAL levers:
  day-expansion gate (only scalp names already up >= DAY% on the session) x trail width.
The validated pump edge ([[pump-scalp-ah-momentum-edge]]) was day-expansion + continuation,
NOT raw ignition. Naive ignition+trail was flat/negative; test selectivity here.
"""
import sys, json, urllib.request, pickle, os, datetime, concurrent.futures as cf

UNIVERSE_FILE="/tmp/bigcap_universe.txt"; CACHE="/tmp/bigcap_5m.pkl"
RANGE="60d"; INTERVAL="5m"; UA={"User-Agent":"Mozilla/5.0"}
LB=6; VOLX=3.0; MAX_HOLD=48; SLIP=0.0008; MIN_PRICE=10.0; IG=3.0

def get(u,t=15): return urllib.request.urlopen(urllib.request.Request(u,headers=UA),timeout=t).read().decode("utf-8","replace")
def bars(sym):
    try:
        j=json.loads(get(f"https://query1.finance.yahoo.com/v8/finance/chart/{sym}?range={RANGE}&interval={INTERVAL}"))
        res=j["chart"]["result"][0]; ts=res["timestamp"]; q=res["indicators"]["quote"][0]
        o,h,l,c,v=q["open"],q["high"],q["low"],q["close"],q["volume"]
        return [(ts[i],o[i],h[i],l[i],c[i],v[i]) for i in range(len(ts)) if None not in (o[i],h[i],l[i],c[i],v[i])]
    except Exception: return []

def load_data():
    if os.path.exists(CACHE):
        d=pickle.load(open(CACHE,"rb")); print(f"# cache {len(d)} syms",file=sys.stderr); return d
    syms=[s.strip() for s in open(UNIVERSE_FILE) if s.strip()]; data={}; done=0
    with cf.ThreadPoolExecutor(max_workers=10) as ex:
        futs={ex.submit(bars,s):s for s in syms}
        for fut in cf.as_completed(futs):
            done+=1
            if done%100==0: print(f"# {done}/{len(syms)}",file=sys.stderr)
            b=fut.result()
            if b: data[futs[fut]]=b
    pickle.dump(data,open(CACHE,"wb")); print(f"# pulled {len(data)}",file=sys.stderr); return data

def day_key(ts): return datetime.datetime.utcfromtimestamp(ts).date()

def sim(b, day_gate, trail):
    n=len(b); trades=[]
    if n<30: return trades
    inpos=False; entry=peak=0.0; hold=0
    cur_day=None; day_open=0.0
    for i in range(21,n):
        ts,o,h,l,c,v=b[i]
        dk=day_key(ts)
        if dk!=cur_day: cur_day=dk; day_open=o   # session open
        if inpos:
            hold+=1
            if h>peak: peak=h
            tstop=peak*(1-trail)
            if l<=tstop: trades.append(tstop*(1-SLIP)/entry-1); inpos=False; continue
            if hold>=MAX_HOLD: trades.append(c*(1-SLIP)/entry-1); inpos=False; continue
            continue
        if c<MIN_PRICE: continue
        if day_open<=0: continue
        day_up=(c/day_open-1)*100
        if day_up<day_gate: continue                 # DAY-EXPANSION gate (the edge)
        avgv=sum(b[k][5] for k in range(i-20,i))/20.0
        if avgv<=0 or v<VOLX*avgv: continue
        base=b[i-LB][4]
        if base<=0 or (c/base-1)*100<IG: continue     # fresh continuation ignition
        entry=c*(1+SLIP); peak=h; hold=0; inpos=True
    return trades

def main():
    data=load_data()
    print(f"\n=== BIG-CAP SCALP SWEEP: day-expansion gate x trail ({len(data)} NAS/SPX, ~2-3mo 5m, IG>={IG}% vol>={VOLX}x slip={SLIP*1e4:.0f}bps) ===")
    print(f"{'dayGate%':>8} {'trail%':>7} {'trades':>7} {'WR%':>6} {'avg%':>7} {'PF':>6} {'tot%':>9}")
    for dg in [0,3,5,8,12,20]:
        for tr in [0.010,0.015,0.020,0.030]:
            allr=[]
            for s,b in data.items(): allr+=sim(b,dg,tr)
            if not allr: print(f"{dg:8} {tr*100:7.1f} {0:7}"); continue
            w=[r for r in allr if r>0]; lo=[r for r in allr if r<=0]
            gw=sum(w); gl=-sum(lo); pf=gw/gl if gl>0 else (99 if gw>0 else 0)
            star=" <--" if pf>=1.3 and len(allr)>=30 else ""
            print(f"{dg:8} {tr*100:7.1f} {len(allr):7} {100*len(w)/len(allr):6.1f} {1e2*sum(allr)/len(allr):7.3f} {pf:6.2f} {1e2*sum(allr):9.1f}{star}")
    print("\n('<--' = PF>=1.3 with n>=30. day-expansion = only scalp names already up >= gate% on the session.)")

if __name__=="__main__": main()
