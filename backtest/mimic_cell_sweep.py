#!/usr/bin/env python3
"""
CELL SWEEP — pick 4 TURTLE-mimic cells + 2 DIP-mimic cells (operator: "with this pf
i want 4 mimics added, add 2 as well"). QQQ>200DMA gated (the #2 winner). Judged
STANDALONE, all-6. Cell convention matches the shipped mimic: lc=arm, pre_arm_be=arm/2
("half-of-arm"), cap=10. Sweeps (arm,gb) and ranks.
"""
import os, csv
from collections import defaultdict

OHLC_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "backtest", "data", "bigcap_daily_ohlc")
DIP_NAMES    = ["MU","NVDA","AVGO","DELL","CRDO","STX","INTC","AMD","AAPL","TPR","MSFT"]
TURTLE_NAMES = ["NVDA","AVGO","STX","DD","AMD","AAPL","TPR","BMY","SWKS","MSFT","QCOM"]
COST_BP = 8.0
DIP_SMA, DIP_RSI_LEN, DIP_RSI_IN = 200, 2, 10.0
TUR_HI_N, TUR_LO_N = 20, 10
ARMS = [1.5, 2.0, 2.5, 3.0, 3.5, 4.0]
GBS  = [0.40, 0.50, 0.60, 0.70]

def load(sym):
    p=os.path.join(OHLC_DIR,f"{sym}.csv"); ds,c=[],[]; o,h,l=[],[],[]
    with open(p) as f:
        r=csv.reader(f); next(r)
        for row in r:
            if len(row)<5: continue
            ds.append(row[0]); o.append(float(row[1])); h.append(float(row[2])); l.append(float(row[3])); c.append(float(row[4]))
    return ds,o,h,l,c

def dip_entries(ds,c):
    ents=[];N=len(c)
    for i in range(N):
        if i<DIP_SMA+1: continue
        sma=sum(c[i-DIP_SMA:i])/DIP_SMA; g=ll=0.0
        for k in range(i-DIP_RSI_LEN+1,i+1):
            ch=c[k]-c[k-1]
            if ch>0: g+=ch
            else: ll+=-ch
        rsi=100.0 if ll==0 else 100.0-100.0/(1.0+g/ll)
        if c[i]>sma and rsi<DIP_RSI_IN: ents.append((i,ds[i],c[i]))
    return ents

def turtle_entries(ds,c):
    ents=[];N=len(c);inp=False
    for i in range(N):
        if i<TUR_HI_N+1: continue
        if not inp:
            if c[i]>max(c[i-TUR_HI_N:i]): ents.append((i,ds[i],c[i])); inp=True
        else:
            if c[i]<min(c[i-TUR_LO_N:i]): inp=False
    return ents

def sim_leg(ei,epx,o,h,l,c,arm,gb,lc,cap,pbe):
    peak=0.0;trough=0.0;armed=False;bars=0;N=len(c);j=ei+1
    while j<N:
        bars+=1
        for p in (l[j],h[j],c[j]):
            ret=(p/epx-1.0)*100.0
            if ret>peak: peak=ret
            if ret<trough: trough=ret
            if not armed:
                if pbe>0 and peak>=pbe:
                    if ret<=0.0: return 0.0
                elif ret<=-lc: return -lc
                if peak>=arm: armed=True
            else:
                if ret<=(1.0-gb)*peak: return (1.0-gb)*peak
        if bars>=cap:
            return (c[j]/epx-1.0)*100.0
        j+=1
    return (c[-1]/epx-1.0)*100.0

def etf_bull(sym):
    ds,o,h,l,c=load(sym); m={}
    for i in range(len(c)):
        m[ds[i]] = (c[i] > sum(c[i-DIP_SMA:i])/DIP_SMA) if i>=DIP_SMA else True
    return m

def clips(names,data,ents,arm,gb,lc,pbe,gate):
    cl=[]
    for sym in names:
        ds,o,h,l,c=data[sym]
        for (ei,ed,epx) in ents[sym]:
            if not gate.get(ed,True): continue
            r=sim_leg(ei,epx,o,h,l,c,arm,gb,lc,10,pbe)-COST_BP/100.0
            cl.append((ed,r))
    return cl

def stats(cl,gate):
    n=len(cl)
    if n==0: return None
    net=sum(r for _,r in cl)
    gp=sum(r for _,r in cl if r>0); gn=-sum(r for _,r in cl if r<0)
    pf=(gp/gn) if gn>1e-9 else 999.0
    wins=sum(1 for _,r in cl if r>1e-9); worst=min(r for _,r in cl)
    sc=sorted(cl); cum=peak=mdd=0.0
    for _,r in sc: cum+=r; peak=max(peak,cum); mdd=min(mdd,cum-peak)
    m=len(sc)//2; h1=sum(r for _,r in sc[:m]); h2=sum(r for _,r in sc[m:])
    # bull/bear by the SAME gate map (gated -> bear~0); keep for completeness
    bull=sum(r for d,r in cl if gate.get(d,True)); bear=sum(r for d,r in cl if not gate.get(d,True))
    viable=(net>0 and pf>=1.3 and h1>0 and h2>0 and bull>0 and (bear>=0))
    return dict(n=n,net=net,pf=pf,wr=100*wins/n,worst=worst,mdd=mdd,h1=h1,h2=h2,viable=viable)

def sweep(fam,names,data,ents,gate):
    rows=[]
    for arm in ARMS:
        for gb in GBS:
            lc=arm; pbe=arm/2.0
            s=stats(clips(names,data,ents,arm,gb,lc,pbe,gate),gate)
            if s: rows.append((arm,gb,lc,pbe,s))
    # rank: viable first, then by net/|mdd| (return-per-drawdown), then PF
    def score(t):
        s=t[4]; rpd = s['net']/abs(s['mdd']) if s['mdd']<0 else s['net']
        return (s['viable'], rpd, s['pf'])
    rows.sort(key=score, reverse=True)
    print(f"\n================ {fam} sweep (QQQ>200DMA gated) — ranked ================")
    print(f"  {'arm':>4} {'gb':>4} {'lc':>4} {'pbe':>4}  {'n':>4} {'net%':>8} {'PF':>5} {'WR%':>5} {'worst':>6} {'mdd%':>7} {'net/mdd':>7} {'viable'}")
    for arm,gb,lc,pbe,s in rows:
        rpd=s['net']/abs(s['mdd']) if s['mdd']<0 else 0
        print(f"  {arm:>4.1f} {gb:>4.2f} {lc:>4.1f} {pbe:>4.1f}  {s['n']:>4} {s['net']:>+8.1f} {s['pf']:>5.2f} {s['wr']:>5.1f} {s['worst']:>+6.2f} {s['mdd']:>+7.1f} {rpd:>7.2f} {'YES' if s['viable'] else 'no'}")
    return rows

def main():
    alln=sorted(set(DIP_NAMES)|set(TURTLE_NAMES))
    data={s:load(s) for s in alln}
    de={s:dip_entries(data[s][0],data[s][4]) for s in DIP_NAMES}
    te={s:turtle_entries(data[s][0],data[s][4]) for s in TURTLE_NAMES}
    qqq=etf_bull("QQQ")
    tr=sweep("TURTLE",TURTLE_NAMES,data,te,qqq)
    dr=sweep("DIP",DIP_NAMES,data,de,qqq)
    print("\n\n>>> pick DIVERSE viable cells (spread arm/gb, avoid near-duplicates)")

if __name__=="__main__": main()
