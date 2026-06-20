#!/usr/bin/env python3
# Gold TOM (turn-of-month) faithful test on long daily history (gcf_daily 2010-2026).
import datetime
from collections import defaultdict
COST=2.0/1e4
def load_ymd(p):
    rows=[]
    for ln in open(p):
        a=ln.strip().split(',')
        if len(a)<5: continue
        try: d=datetime.datetime.strptime(a[0],"%Y-%m-%d").date(); o,h,l,c=map(float,a[1:5])
        except: continue
        if c>0: rows.append((d,o,h,l,c))
    rows.sort(); return rows
def tom(rows,ln,fn):
    n=len(rows); m=defaultdict(list)
    for i,r in enumerate(rows): m[(r[0].year,r[0].month)].append(i)
    inw=[False]*n; ks=sorted(m)
    for mi,mk in enumerate(ks):
        for i in m[mk][-ln:]: inw[i]=True
        if mi+1<len(ks):
            for i in m[ks[mi+1]][:fn]: inw[i]=True
    tr=[]; i=1
    while i<n-1:
        if inw[i] and not inw[i-1]:
            j=i
            while j<n and inw[j]: j+=1
            if min(j,n-1)>i and rows[i][1]>0: tr.append((rows[i][0],(rows[min(j,n-1)][1]/rows[i][1]-1)-COST))
            i=j
        else: i+=1
    return tr
def pf(tr):
    w=sum(r for _,r in tr if r>0); l=-sum(r for _,r in tr if r<0); return w/l if l>0 else 9.9
rows=load_ymd("/Users/jo/Tick/mid_freq_research/gcf_daily.csv")
print(f"GOLD gcf_daily {rows[0][0]}..{rows[-1][0]} n={len(rows)}")
for ln,fn in [(3,3),(2,3),(1,3)]:
    tr=tom(rows,ln,fn); h=len(tr)//2
    bull=[t for t in tr if t[0].year!=2022]; bear=[t for t in tr if t[0].year==2022]
    print(f" last{ln}+first{fn}: n={len(tr)} PF{pf(tr):.2f} H1{pf(tr[:h]):.2f} H2{pf(tr[h:]):.2f} BULL{pf(bull):.2f} BEAR{pf(bear):.2f}")
