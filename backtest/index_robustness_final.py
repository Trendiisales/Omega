#!/usr/bin/env python3
# Final robustness: for the daily long-only Donchian fleet, report half-split
# (H1 2016-2020 vs H2 2021-2026) AND the 2022-bear behaviour, to separate
# "trend filter protects by staying OUT in the bear" from bull-beta.
import datetime
from collections import defaultdict
BASE='/Users/jo/Tick/'; COST_BP=0.75/10000.0
INDICES={'SPX':'SPX_daily_2016_2026.csv','NDX':'NDX_daily_2016_2026.csv',
         'DJ30':'DJ30_daily_2016_2026.csv','GER40':'GER40_daily_2016_2026.csv','UK100':'UK100_daily_2016_2026.csv'}

def load(p):
    r=[]
    for line in open(p):
        x=line.strip().split(',')
        if len(x)<5: continue
        ts=int(x[0]);o,h,l,c=map(float,x[1:5])
        y=datetime.datetime.fromtimestamp(ts,datetime.timezone.utc).year
        r.append((ts,o,h,l,c,y))
    return r

def donch_lo(rows,N):
    pos=0;entry=0.0;ey=None;tr=[]
    for i in range(N+1,len(rows)):
        c=rows[i][4]
        hi=max(rows[k][2] for k in range(i-N,i));lo=min(rows[k][3] for k in range(i-N,i))
        if pos==1 and c<lo: tr.append((c/entry-1-2*COST_BP,ey));pos=0
        if pos==0 and c>hi: pos=1;entry=c;ey=rows[i][5]
    if pos==1: tr.append((rows[-1][4]/entry-1-2*COST_BP,ey))
    return tr

def pf(x):
    gp=sum(t for t in x if t>0);gl=-sum(t for t in x if t<0)
    return gp/gl if gl>0 else float('inf')

print("DAILY LONG-ONLY DONCHIAN — half-split robustness + 2022 bear behaviour")
print("Half1=2016-2020, Half2=2021-2026. ROBUST = both halves PF>1 AND 2022 not a big loss.\n")
print(f"{'idx':6s} {'N':>3s} {'fullPF':>7s} {'net%':>6s} {'n':>4s} {'H1pf':>5s} {'H2pf':>5s} {'2022net%':>8s} {'2022n':>5s}  verdict")
best={}
for name,fn in INDICES.items():
    rows=load(BASE+fn)
    for N in (20,40,55):
        tr=donch_lo(rows,N)
        full=pf([t for t,_ in tr]); net=sum(t for t,_ in tr)
        h1=[t for t,y in tr if y<=2020]; h2=[t for t,y in tr if y>=2021]
        y22=[t for t,y in tr if y==2022]
        v="ROBUST" if (pf(h1)>1 and pf(h2)>1 and sum(y22)>-0.05) else "bull-tilt" if full>1.3 else "weak"
        print(f"{name:6s} {N:3d} {full:7.2f} {net*100:+6.0f} {len(tr):4d} {pf(h1):5.2f} {pf(h2):5.2f} {sum(y22)*100:+8.1f} {len(y22):5d}  {v}")
        key=name
        if key not in best or full>best[key][1]:
            if v in ("ROBUST","bull-tilt"): best[key]=(N,full,net,v)
    print()
print("BEST robust/bull-tilt N per index:")
for k,v in best.items():
    print(f"  {k:6s} Donch{v[0]}L  PF={v[1]:.2f} net={v[2]*100:+.0f}%  ({v[3]})")
