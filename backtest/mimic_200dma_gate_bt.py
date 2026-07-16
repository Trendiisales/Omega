#!/usr/bin/env python3
"""
#2 — SPY/QQQ >200DMA REGIME GATE on the stock mimics (unifies with #1).

Operator #2: add a broad-market 200DMA bull-gate to the DIP mimic. #1 (TURTLE-mimic)
came back bull-good / bear-bad -> the operator's standing rule is BULL-GATE, never
reject (feedback-companion-bull-gate-not-reject). Both are the same lever, so this
tests it on BOTH families at once.

Gate = only mirror an entry when the broad market is in a bull regime on the entry
date: SPY close > SPY SMA200 (and, alternatively, QQQ close > QQQ SMA200). 200DMA
bull-gating is fine for EQUITIES (the crypto-only ban, feedback-no-200dma-crypto,
does not apply here).

Families / entries FAITHFUL to include/StockDipTurtleEngine.hpp:
  DIP    : close>SMA200 AND Cutler-RSI2<10  (shipped stockdip_mimic_prearm_floor.py style)
  TURTLE : flat-gated 20-close-high entry / 10-close-low exit (real one-position cadence)
Mimic cells = SHIPPED (1793e0b3): T arm2/gb50/lc2/pbe1.0, W arm3/gb70/lc3/pbe1.5.
Judged STANDALONE, all-6 (net>0, PF>=1.3, both halves+, bull+, bear+).
"""
import os, csv
from collections import defaultdict

OHLC_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "backtest", "data", "bigcap_daily_ohlc")
DIP_NAMES    = ["MU","NVDA","AVGO","DELL","CRDO","STX","INTC","AMD","AAPL","TPR","MSFT"]
TURTLE_NAMES = ["NVDA","AVGO","STX","DD","AMD","AAPL","TPR","BMY","SWKS","MSFT","QCOM"]
COST_BP = 8.0
DIP_SMA, DIP_RSI_LEN, DIP_RSI_IN = 200, 2, 10.0
TUR_HI_N, TUR_LO_N = 20, 10
CELLS = [("T",2.0,0.50,2.0,1.0), ("W",3.0,0.70,3.0,1.5)]

def load(sym):
    p=os.path.join(OHLC_DIR,f"{sym}.csv"); ds,o,h,l,c=[],[],[],[],[]
    with open(p) as f:
        r=csv.reader(f); next(r)
        for row in r:
            if len(row)<5: continue
            ds.append(row[0]); o.append(float(row[1])); h.append(float(row[2])); l.append(float(row[3])); c.append(float(row[4]))
    return ds,o,h,l,c

def dip_entries(ds,c):
    ents=[]; N=len(c)
    for i in range(N):
        if i<DIP_SMA+1: continue
        sma=sum(c[i-DIP_SMA:i])/DIP_SMA
        g=ll=0.0
        for k in range(i-DIP_RSI_LEN+1,i+1):
            ch=c[k]-c[k-1]
            if ch>0: g+=ch
            else: ll+=-ch
        rsi=100.0 if ll==0 else 100.0-100.0/(1.0+g/ll)
        if c[i]>sma and rsi<DIP_RSI_IN: ents.append((i,ds[i],c[i]))
    return ents

def turtle_entries(ds,c):
    ents=[]; N=len(c); in_pos=False
    for i in range(N):
        if i<TUR_HI_N+1: continue
        if not in_pos:
            if c[i]>max(c[i-TUR_HI_N:i]): ents.append((i,ds[i],c[i])); in_pos=True
        else:
            if c[i]<min(c[i-TUR_LO_N:i]): in_pos=False
    return ents

def sim_leg(ei,epx,o,h,l,c,arm_pct,gb,lc_pct,cap_bars,pre_arm_be_pct=0.0):
    peak=0.0;trough=0.0;armed=False;bars=0;N=len(c);j=ei+1
    while j<N:
        bars+=1
        for p in (l[j],h[j],c[j]):
            ret=(p/epx-1.0)*100.0
            if ret>peak: peak=ret
            if ret<trough: trough=ret
            if not armed:
                if pre_arm_be_pct>0 and peak>=pre_arm_be_pct:
                    if ret<=0.0: return (0.0,"BE_FLOOR",bars,peak,trough)
                elif ret<=-lc_pct:
                    return (-lc_pct,"LOSS_CUT",bars,peak,trough)
                if peak>=arm_pct: armed=True
            else:
                stop=(1.0-gb)*peak
                if ret<=stop: return (stop,"TRAIL_STOP",bars,peak,trough)
        if bars>=cap_bars:
            ret=(c[j]/epx-1.0)*100.0; return (ret,"WINDOW_CAP",bars,peak,min(trough,ret))
        j+=1
    ret=(c[-1]/epx-1.0)*100.0; return (ret,"EOD_FLUSH",bars,peak,min(trough,ret))

def etf_bull_map(sym):
    """date -> (close > SMA200) for the ETF; the broad-market regime gate."""
    ds,o,h,l,c=load(sym); m={}
    for i in range(len(c)):
        m[ds[i]] = (c[i] > sum(c[i-DIP_SMA:i])/DIP_SMA) if i>=DIP_SMA else True
    return m

def stats(cl):
    n=len(cl)
    if n==0: return dict(n=0,net=0.0,pf=0.0,wr=0.0,worst=0.0,avg=0.0,mdd=0.0)
    net=sum(x['r'] for x in cl)
    gp=sum(x['r'] for x in cl if x['r']>0); gn=-sum(x['r'] for x in cl if x['r']<0)
    pf=(gp/gn) if gn>1e-9 else float('inf')
    wins=sum(1 for x in cl if x['r']>1e-9); worst=min(x['r'] for x in cl)
    sc=sorted(cl,key=lambda x:x['date']); cum=peak=mdd=0.0
    for x in sc: cum+=x['r']; peak=max(peak,cum); mdd=min(mdd,cum-peak)
    return dict(n=n,net=net,pf=pf,wr=100.0*wins/n,worst=worst,avg=net/n,mdd=mdd)

def half(cl):
    sc=sorted(cl,key=lambda x:x['date']); m=len(sc)//2; return sc[:m],sc[m:]
def pf_s(s): return "inf" if s['pf']==float('inf') else f"{s['pf']:.2f}"
def fmt(s):
    if s['n']==0: return "n=0"
    return f"n={s['n']:4d} net={s['net']:+8.1f}% PF={pf_s(s):>4} WR={s['wr']:4.1f}% worst={s['worst']:+6.2f}% mdd={s['mdd']:+7.1f}%"

def build_clips(names,data,ents,arm,gb,lc,be,gate=None):
    """gate: dict date->bull, or None (ungated). Also tag each clip's bull for reporting."""
    cl=[]
    for sym in names:
        ds,o,h,l,c=data[sym]
        for (ei,ed,epx) in ents[sym]:
            bull = gate.get(ed,True) if gate else True
            if gate and not bull: continue          # gated out
            r,reason,hold,mfe,mae=sim_leg(ei,epx,o,h,l,c,arm,gb,lc,10,be)
            cl.append(dict(sym=sym,date=ed,r=r-COST_BP/100.0,reason=reason,bull=bull))
    return cl

def report(tag,names,data,ents,arm,gb,lc,be,gate,gatemap_for_bull):
    cl=build_clips(names,data,ents,arm,gb,lc,be,gate=gate)
    # bull/bear split always judged by the SPY/QQQ gate map (even ungated) to show what the gate removes
    for x in cl: x['bull']=gatemap_for_bull.get(x['date'],True)
    s=stats(cl); h1,h2=half(cl); s1=stats(h1); s2=stats(h2)
    sb=stats([x for x in cl if x['bull']]); sr=stats([x for x in cl if not x['bull']])
    viable=(s['net']>0 and s['pf']>=1.3 and s1['net']>0 and s2['net']>0 and sb['net']>0 and (sr['n']==0 or sr['net']>0))
    print(f"  {tag:22s} {fmt(s)}")
    print(f"      H1:{s1['net']:+.0f} H2:{s2['net']:+.0f}  BULL:{sb['net']:+.0f}({sb['n']}) BEAR:{sr['net']:+.0f}({sr['n']})  -> {'VIABLE' if viable else 'NOT VIABLE'}")
    return viable

def main():
    alln=sorted(set(DIP_NAMES)|set(TURTLE_NAMES))
    data={s:load(s) for s in alln}
    de={s:dip_entries(data[s][0],data[s][4]) for s in DIP_NAMES}
    te={s:turtle_entries(data[s][0],data[s][4]) for s in TURTLE_NAMES}
    spy=etf_bull_map("SPY"); qqq=etf_bull_map("QQQ")
    print(f"DIP entries={sum(len(de[s]) for s in DIP_NAMES)}  TURTLE entries={sum(len(te[s]) for s in TURTLE_NAMES)}")

    for fam,names,ents,bullmap in (("DIP",DIP_NAMES,de,spy),("TURTLE",TURTLE_NAMES,te,spy)):
        for cn,arm,gb,lc,be in CELLS:
            print(f"\n===== {fam} family — cell {cn} (arm{arm}/gb{gb}/lc{lc}/pbe{be}) =====")
            report("ungated",       names,data,ents,arm,gb,lc,be, None, spy)
            report("SPY>200DMA gate",names,data,ents,arm,gb,lc,be, spy,  spy)
            report("QQQ>200DMA gate",names,data,ents,arm,gb,lc,be, qqq,  qqq)

if __name__=="__main__": main()
