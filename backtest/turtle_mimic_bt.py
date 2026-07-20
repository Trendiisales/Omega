#!/usr/bin/env python3
"""
#1 — TURTLE-BREAKOUT MIMIC study (extend the shipped StockDipMimic to the TURTLE family).

TODAY: StockDipMimic (GoldTrendMimicBook) runs on the DIP family only. The TURTLE
breakout family (live, profitable) has NO mimic. This tests adding the SAME BE-floored
mimic to TURTLE entries. Independent SHADOW book — judged STANDALONE (not vs the real
engine; a companion coexists and is additive — feedback-companion-independent-engine).

FAITHFUL entry cadence = the REAL StockDipTurtleEngine TURTLE archetype
(include/StockDipTurtleEngine.hpp): LONG at the daily close when close > max(prior 20
closes); the real position is held (one at a time) and closes at the first close <
min(prior 10 closes). The mimic opens a leg mirroring each REAL entry, then runs its
OWN arm/trail/BE-floor exit (GoldTrendMimicBook.on_h1_bar port, identical to the
shipped stockdip_mimic_prearm_floor.py sim_leg).

Mimic cells = the SHIPPED params (commit 1793e0b3):
  T (Tight): arm=2.0 gb=0.50 lc=2.0 cap=10 pre_arm_be=1.0
  W (Wide) : arm=3.0 gb=0.70 lc=3.0 cap=10 pre_arm_be=1.5
"""
import os, csv
from collections import defaultdict

OHLC_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "backtest", "data", "bigcap_daily_ohlc")
NAMES = ["NVDA","AVGO","STX","DD","AMD","AAPL","TPR","BMY","SWKS","MSFT","QCOM"]
COST_BP = 8.0
TUR_HI_N, TUR_LO_N = 20, 10          # real engine: 20-close high entry / 10-close low exit

def load(sym):
    p = os.path.join(OHLC_DIR, f"{sym}.csv")
    ds,o,h,l,c = [],[],[],[],[]
    with open(p) as f:
        r=csv.reader(f); next(r)
        for row in r:
            if len(row)<5: continue
            ds.append(row[0]); o.append(float(row[1])); h.append(float(row[2]))
            l.append(float(row[3])); c.append(float(row[4]))
    return ds,o,h,l,c

def turtle_entries(ds,c):
    """Real StockTurtle cadence: enter when FLAT and close>max(prior 20 closes);
       hold until close<min(prior 10 closes); only then eligible to re-enter.
       Returns the entry bars (ei, date, entry_px) the mimic mirrors."""
    ents=[]; N=len(c); in_pos=False
    for i in range(N):
        if i < TUR_HI_N+1: continue
        hi20 = max(c[i-TUR_HI_N:i])          # highest of prior 20 closes (excl. today)
        lo10 = min(c[i-TUR_LO_N:i])          # lowest of prior 10 closes (excl. today)
        if not in_pos:
            if c[i] > hi20:
                ents.append((i, ds[i], c[i])); in_pos=True
        else:
            if c[i] < lo10:
                in_pos=False
    return ents

def sim_leg(ei, epx, o,h,l,c, arm_pct, gb, lc_pct, cap_bars, pre_arm_be_pct=0.0):
    """Port of GoldTrendMimicBook.on_h1_bar (long) + PRE-ARM BE-ratchet.
       Identical to the shipped stockdip_mimic_prearm_floor.py sim_leg."""
    peak=0.0; trough=0.0; armed=False; bars=0; N=len(c); j=ei+1
    while j<N:
        bars+=1
        seq=(l[j],h[j],c[j])                 # adverse (low) first for a long
        for p in seq:
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
            ret=(c[j]/epx-1.0)*100.0
            return (ret,"WINDOW_CAP",bars,peak,min(trough,ret))
        j+=1
    ret=(c[-1]/epx-1.0)*100.0
    return (ret,"EOD_FLUSH",bars,peak,min(trough,ret))

def build_regime(data):
    """Equal-weight index of the family; bull = idx >= its own SMA200 (same as the DIP harness)."""
    bydate=defaultdict(dict)
    for sym,(ds,o,h,l,c) in data.items():
        for i,d in enumerate(ds): bydate[d][sym]=c[i]
    dates=sorted(bydate.keys()); firsts={}; idx=[]
    for d in dates:
        vals=[]
        for sym in NAMES:
            if sym in bydate[d]:
                firsts.setdefault(sym,bydate[d][sym]); vals.append(bydate[d][sym]/firsts[sym])
        idx.append(sum(vals)/len(vals) if vals else float('nan'))
    reg={}
    for i,d in enumerate(dates):
        reg[d]= True if i<200 else idx[i]>=sum(idx[i-200:i])/200.0
    return reg

def run(data, ents, reg, arm,gb,lc,cap, be):
    clips=[]
    for sym in NAMES:
        ds,o,h,l,c=data[sym]
        for (ei,ed,epx) in ents[sym]:
            ret,reason,hold,mfe,mae=sim_leg(ei,epx,o,h,l,c,arm,gb,lc,cap,be)
            clips.append(dict(sym=sym,date=ed,ret_real=ret-COST_BP/100.0,reason=reason,mfe=mfe,mae=mae,bull=reg.get(ed,True)))
    return clips

def stats(cl):
    n=len(cl)
    if n==0: return dict(n=0,net=0)
    net=sum(x['ret_real'] for x in cl)
    gp=sum(x['ret_real'] for x in cl if x['ret_real']>0); gn=-sum(x['ret_real'] for x in cl if x['ret_real']<0)
    pf=(gp/gn) if gn>1e-9 else float('inf')
    wins=sum(1 for x in cl if x['ret_real']>1e-9); worst=min(x['ret_real'] for x in cl)
    sc=sorted(cl,key=lambda x:x['date']); cum=peak=mdd=0.0
    for x in sc:
        cum+=x['ret_real']; peak=max(peak,cum); mdd=min(mdd,cum-peak)
    return dict(n=n,net=net,pf=pf,wr=100.0*wins/n,worst=worst,avg=net/n,mdd=mdd)

def sub(cl,f): return [x for x in cl if f(x)]
def yr(cl,y): return [x for x in cl if x['date'].startswith(y)]
def half(cl):
    sc=sorted(cl,key=lambda x:x['date']); m=len(sc)//2; return sc[:m],sc[m:]

def fmt(s):
    if s.get('n',0)==0: return "n=0"
    pf="inf" if s['pf']==float('inf') else f"{s['pf']:.2f}"
    return f"n={s['n']:4d} net={s['net']:+8.1f}% avg={s['avg']:+5.2f}% PF={pf:>4} WR={s['wr']:4.1f}% worst={s['worst']:+6.2f}% mdd={s['mdd']:+7.1f}%"

def report(tag, data, ents, reg, arm,gb,lc,cap, be, bull_gate=False):
    cl=run(data,ents,reg,arm,gb,lc,cap,be)
    if bull_gate: cl=[x for x in cl if x['bull']]   # SPY/universe-200DMA bull-gate: drop bear-entry clips
    s=stats(cl); h1,h2=half(cl); s1=stats(h1); s2=stats(h2)
    sb=stats(sub(cl,lambda x:x['bull'])); sr=stats(sub(cl,lambda x:not x['bull'])); y22=stats(yr(cl,"2022"))
    rc=defaultdict(lambda:[0,0.0])
    for x in cl: rc[x['reason']][0]+=1; rc[x['reason']][1]+=x['ret_real']
    viable=(s['net']>0 and s['pf']>=1.3 and s1['net']>0 and s2['net']>0 and sb['net']>0 and (sr.get('n',0)==0 or sr['net']>0))
    print(f"\n--- {tag}  arm={arm} gb={gb} lc={lc} cap={cap} pre_arm_be={be} ---")
    print(f"  POOLED: {fmt(s)}")
    print(f"  H1:{s1['net']:+.0f}  H2:{s2['net']:+.0f}  BULL:{sb['net']:+.0f}({sb.get('n',0)})  BEAR:{sr.get('net',0):+.0f}({sr.get('n',0)})  2022:{y22.get('net',0):+.0f}")
    print("  exits: "+" | ".join(f"{k}:{v[0]}({v[1]:+.0f}%)" for k,v in sorted(rc.items())))
    print(f"  VERDICT: {'VIABLE' if viable else 'NOT VIABLE'} (all-6: net{s['net']>0} PF>=1.3:{s['pf']>=1.3} H1+{s1['net']>0} H2+{s2['net']>0} bull+{sb['net']>0} bear+{sr.get('net',0)>0 or sr.get('n',0)==0})")
    return s

def main():
    data={}; ents={}
    for sym in NAMES:
        d=load(sym); data[sym]=d; ents[sym]=turtle_entries(d[0],d[4])
    reg=build_regime(data)
    tot=sum(len(ents[s]) for s in NAMES)
    print(f"TURTLE-MIMIC  names={len(NAMES)}  total_entries={tot}  per-name:")
    print("  "+"  ".join(f"{s}:{len(ents[s])}" for s in NAMES))
    LIVE=(("A",1.5,0.50,2.0,0.75),("B",2.0,0.50,2.0,1.0),("C",2.5,0.40,2.0,1.25),("D",3.5,0.40,2.0,1.75))
    print("\n########## UNGATED (baseline — the NOT-VIABLE finding) ##########")
    for label,arm,gb,lc,be in LIVE:
        report(f"TURTLE-mimic {label} UNGATED", data,ents,reg, arm,gb,lc,10, be)
    print("\n########## BULL-GATED (SPY/universe-200DMA — shipped S-2026-07-20az) ##########")
    for label,arm,gb,lc,be in LIVE:
        report(f"TURTLE-mimic {label} BULL-GATE", data,ents,reg, arm,gb,lc,10, be, bull_gate=True)

if __name__=="__main__": main()
