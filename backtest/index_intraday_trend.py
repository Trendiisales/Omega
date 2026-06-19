#!/usr/bin/env python3
# Intraday H1 index TREND test. Honest skepticism: NqMomentum was bull-biased
# DEAD intraday. Question: is there a cost-survivable intraday index trend, or
# is index trend daily-only? Cost: index ~1bp/side intraday (slightly higher
# than daily due to intraday spread). Per-year buckets, esp. bear slices.
import sys, datetime
from collections import defaultdict
COST_BP=1.0/10000.0   # 1bp/side intraday (conservative for index CFD)

def load(path):
    rows=[]
    with open(path) as f:
        for line in f:
            p=line.strip().split(',')
            if len(p)<5: continue
            try: ts=int(p[0]); o,h,l,c=map(float,p[1:5])
            except: continue
            d=datetime.datetime.fromtimestamp(ts,datetime.timezone.utc)
            rows.append((ts,o,h,l,c,d.year,d.month,d.hour))
    return rows

def donch(rows,N,allow_short):
    pos=0;entry=0.0;ey=None;tr=[]
    for i in range(N+1,len(rows)):
        c=rows[i][4]
        hi=max(rows[k][2] for k in range(i-N,i)); lo=min(rows[k][3] for k in range(i-N,i))
        if pos==1 and c<lo: tr.append((c/entry-1-2*COST_BP,ey,rows[i][6]));pos=0
        elif pos==-1 and c>hi: tr.append((entry/c-1-2*COST_BP,ey,rows[i][6]));pos=0
        if pos==0:
            if c>hi: pos=1;entry=c;ey=(rows[i][5],rows[i][6])
            elif allow_short and c<lo: pos=-1;entry=c;ey=(rows[i][5],rows[i][6])
    if pos==1: tr.append((rows[-1][4]/entry-1-2*COST_BP,ey,rows[-1][6]))
    elif pos==-1: tr.append((entry/rows[-1][4]-1-2*COST_BP,ey,rows[-1][6]))
    return tr

def ema(rows,n):
    e=[None]*len(rows);k=2/(n+1);pv=None
    for i,r in enumerate(rows):
        pv=r[4] if pv is None else r[4]*k+pv*(1-k); e[i]=pv
    return e

def macross(rows,fast,slow,allow_short):
    ef=ema(rows,fast);es=ema(rows,slow)
    pos=0;entry=0.0;ey=None;tr=[]
    for i in range(slow+1,len(rows)):
        c=rows[i][4];bull=ef[i]>es[i]
        if pos==1 and not bull: tr.append((c/entry-1-2*COST_BP,ey,rows[i][6]));pos=0
        elif pos==-1 and bull: tr.append((entry/c-1-2*COST_BP,ey,rows[i][6]));pos=0
        if pos==0:
            if bull: pos=1;entry=c;ey=(rows[i][5],rows[i][6])
            elif allow_short and not bull: pos=-1;entry=c;ey=(rows[i][5],rows[i][6])
    if pos==1: tr.append((rows[-1][4]/entry-1-2*COST_BP,ey,rows[-1][6]))
    elif pos==-1: tr.append((entry/rows[-1][4]-1-2*COST_BP,ey,rows[-1][6]))
    return tr

def stats(tr):
    if not tr: return None
    gp=sum(t for t,_,_ in tr if t>0);gl=-sum(t for t,_,_ in tr if t<0)
    pf=gp/gl if gl>0 else float('inf');net=sum(t for t,_,_ in tr)
    wr=sum(1 for t,_,_ in tr if t>0)/len(tr)
    return pf,net,len(tr),wr

def label(path,name):
    rows=load(path)
    if not rows: print(f"{name}: no rows"); return
    bh=rows[-1][4]/rows[0][4]-1
    print(f"\n### {name}  bars={len(rows)}  span {rows[0][5]}-{rows[-1][5]}  buy&hold={bh*100:+.1f}%")
    for N in (20,40,55):
        for sh in (False,True):
            s=stats(donch(rows,N,sh))
            if s: print(f"  Donch{N}{'+S' if sh else 'L':2s} PF={s[0]:5.2f} net={s[1]*100:+7.1f}% n={s[2]:4d} wr={s[3]*100:2.0f}%")
    for f,sl in ((20,100),(50,200)):
        for sh in (False,True):
            s=stats(macross(rows,f,sl,sh))
            if s: print(f"  MA{f}/{sl}{'+S' if sh else 'L':2s} PF={s[0]:5.2f} net={s[1]*100:+7.1f}% n={s[2]:4d} wr={s[3]*100:2.0f}%")

print("="*90)
print("INTRADAY H1 INDEX TREND  (cost 1bp/side)")
print("="*90)
label('/Users/jo/Tick/US500_2426_h1.csv','US500 H1 2024-2026 (CLEAN, bull-ish span)')
label('/Users/jo/Tick/NAS_2022_05_h1.csv','NAS H1 2022-05 (BEAR spot-check, 1 month only)')
