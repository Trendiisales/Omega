#!/usr/bin/env python3
# Does day-of-week seasonality (Tue/Fri long, Wed short) OVERLAP with the
# Donch20 trend cell, or is it ADDITIVE? Measure: (1) standalone seasonal edge,
# (2) what fraction of seasonal long-day returns occur WHILE the trend cell is
# already long (overlap), (3) seasonal edge CONDITIONED on trend being flat/out.
import datetime
from collections import defaultdict
BASE='/Users/jo/Tick/'
COST_BP=0.75/10000.0
INDICES={'SPX':'SPX_daily_2016_2026.csv','NDX':'NDX_daily_2016_2026.csv','DJ30':'DJ30_daily_2016_2026.csv'}

def load(path):
    rows=[]
    with open(path) as f:
        for line in f:
            p=line.strip().split(',')
            if len(p)<5: continue
            ts=int(p[0]); o,hh,l,c=map(float,p[1:5])
            d=datetime.datetime.fromtimestamp(ts,datetime.timezone.utc)
            rows.append((ts,o,hh,l,c,d.year,d.weekday()))
    return rows

def donch_state(rows,N):
    """Return per-bar position state (1=long else 0) for Donch{N} long-only."""
    state=[0]*len(rows); pos=0
    for i in range(N+1,len(rows)):
        c=rows[i][4]
        hi=max(rows[k][2] for k in range(i-N,i)); lo=min(rows[k][3] for k in range(i-N,i))
        if pos==1 and c<lo: pos=0
        if pos==0 and c>hi: pos=1
        state[i]=pos
    return state

for name,fn in INDICES.items():
    rows=load(BASE+fn)
    st=donch_state(rows,20)
    # next-day close-to-close return given today is weekday wd
    # seasonal LONG on Tue(1)/Fri(4) entered at today close, exit next close
    seas_all=[]; seas_when_flat=[]; overlap_days=0; seas_days=0
    for i in range(21,len(rows)-1):
        wd=rows[i][6]
        ret=rows[i+1][4]/rows[i][4]-1
        if wd in (1,4):   # Tue, Fri long
            seas_days+=1
            seas_all.append(ret-2*COST_BP)
            if st[i]==1: overlap_days+=1   # trend already long that day
            else: seas_when_flat.append(ret-2*COST_BP)
        if wd==2:         # Wed short
            seas_all.append(-ret-2*COST_BP)
            if st[i]==0: seas_when_flat.append(-ret-2*COST_BP)
    def pf(x):
        gp=sum(t for t in x if t>0); gl=-sum(t for t in x if t<0)
        return gp/gl if gl>0 else float('inf')
    print(f"\n### {name}")
    print(f"  seasonal(all) net={sum(seas_all)*100:+.0f}% PF={pf(seas_all):.2f} n={len(seas_all)}")
    print(f"  Tue/Fri long days: {seas_days}, of which trend ALREADY long (overlap): {overlap_days} ({overlap_days/seas_days*100:.0f}%)")
    print(f"  seasonal WHEN trend flat/out (additive slice): net={sum(seas_when_flat)*100:+.0f}% PF={pf(seas_when_flat):.2f} n={len(seas_when_flat)}")
