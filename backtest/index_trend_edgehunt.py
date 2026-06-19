#!/usr/bin/env python3
# INDEX DAILY TREND EDGE HUNT — Donchian / Keltner / MA-cross, cross-regime.
# Per-cell: PF, net, n, per-year buckets (CRITICAL: 2022 bear). Cost applied.
# Data: epoch_seconds,O,H,L,C daily bars.
import csv, sys, datetime, statistics
from collections import defaultdict

COST_BP = 0.75 / 10000.0   # ~0.75 bp/side round-trip applied per side (index)

def load(path):
    rows=[]
    with open(path) as f:
        for line in f:
            p=line.strip().split(',')
            if len(p)<5: continue
            try:
                ts=int(p[0]); o,h,l,c=map(float,p[1:5])
            except: continue
            yr=datetime.datetime.utcfromtimestamp(ts).year
            wd=datetime.datetime.utcfromtimestamp(ts).weekday()
            rows.append((ts,o,h,l,c,yr,wd))
    return rows

def atr(rows,i,n):
    if i<n: return None
    s=0.0
    for k in range(i-n+1,i+1):
        h=rows[k][2]; l=rows[k][3]; pc=rows[k-1][4]
        s+=max(h-l, abs(h-pc), abs(l-pc))
    return s/n

def ema_series(rows, n):
    e=[None]*len(rows); k=2/(n+1); prev=None
    for i,r in enumerate(rows):
        c=r[4]
        prev = c if prev is None else c*k+prev*(1-k)
        e[i]=prev
    return e

def run_donchian(rows, N, allow_short, ema_n):
    """Donchian channel breakout. Enter long on close > prior N-day high;
    exit long on close < prior N-day low (and vice versa for short).
    ema_n: if set, only long when close>ema, only short when close<ema."""
    ema = ema_series(rows, ema_n) if ema_n else None
    pos=0; entry=0.0; trades=[]   # (pnl_frac, year)
    entry_year=None
    for i in range(N+1, len(rows)):
        c=rows[i][4]
        hi=max(rows[k][2] for k in range(i-N,i))   # prior N highs
        lo=min(rows[k][3] for k in range(i-N,i))
        long_ok  = (ema is None) or (c>ema[i])
        short_ok = (ema is None) or (c<ema[i])
        # exits
        if pos==1 and c<lo:
            trades.append((c/entry-1-2*COST_BP, entry_year)); pos=0
        elif pos==-1 and c>hi:
            trades.append((entry/c-1-2*COST_BP, entry_year)); pos=0
        # entries
        if pos==0:
            if c>hi and long_ok:
                pos=1; entry=c; entry_year=rows[i][5]
            elif allow_short and c<lo and short_ok:
                pos=-1; entry=c; entry_year=rows[i][5]
    # close final
    if pos==1: trades.append((rows[-1][4]/entry-1-2*COST_BP, entry_year))
    elif pos==-1: trades.append((entry/rows[-1][4]-1-2*COST_BP, entry_year))
    return trades

def run_keltner(rows, ema_n, k_mult, atr_n, allow_short):
    """Keltner breakout: enter long on close > ema + k*ATR, exit on close<ema.
    Long-trend variant."""
    ema=ema_series(rows,ema_n)
    pos=0; entry=0.0; entry_year=None; trades=[]
    for i in range(max(ema_n,atr_n)+1,len(rows)):
        c=rows[i][4]; a=atr(rows,i,atr_n)
        if a is None: continue
        up=ema[i]+k_mult*a; dn=ema[i]-k_mult*a
        if pos==1 and c<ema[i]:
            trades.append((c/entry-1-2*COST_BP, entry_year)); pos=0
        elif pos==-1 and c>ema[i]:
            trades.append((entry/c-1-2*COST_BP, entry_year)); pos=0
        if pos==0:
            if c>up:
                pos=1; entry=c; entry_year=rows[i][5]
            elif allow_short and c<dn:
                pos=-1; entry=c; entry_year=rows[i][5]
    if pos==1: trades.append((rows[-1][4]/entry-1-2*COST_BP, entry_year))
    elif pos==-1: trades.append((entry/rows[-1][4]-1-2*COST_BP, entry_year))
    return trades

def run_macross(rows, fast, slow, allow_short):
    """MA crossover trend: long when fast ema>slow ema, flip/flat on cross."""
    ef=ema_series(rows,fast); es=ema_series(rows,slow)
    pos=0; entry=0.0; entry_year=None; trades=[]
    for i in range(slow+1,len(rows)):
        c=rows[i][4]
        bull = ef[i]>es[i]
        if pos==1 and not bull:
            trades.append((c/entry-1-2*COST_BP, entry_year)); pos=0
        elif pos==-1 and bull:
            trades.append((entry/c-1-2*COST_BP, entry_year)); pos=0
        if pos==0:
            if bull:
                pos=1; entry=c; entry_year=rows[i][5]
            elif allow_short and not bull:
                pos=-1; entry=c; entry_year=rows[i][5]
    if pos==1: trades.append((rows[-1][4]/entry-1-2*COST_BP, entry_year))
    elif pos==-1: trades.append((entry/rows[-1][4]-1-2*COST_BP, entry_year))
    return trades

def stats(trades):
    if not trades: return None
    gp=sum(t for t,_ in trades if t>0); gl=-sum(t for t,_ in trades if t<0)
    pf= gp/gl if gl>0 else float('inf')
    net=sum(t for t,_ in trades)
    wr=sum(1 for t,_ in trades if t>0)/len(trades)
    by=defaultdict(lambda:[0.0,0]) # year->[net,count]
    for t,y in trades:
        by[y][0]+=t; by[y][1]+=1
    return pf, net, len(trades), wr, by

def fmt_year(by):
    out=[]
    for y in sorted(by):
        out.append(f"{y}:{by[y][0]*100:+.0f}%/{by[y][1]}")
    return " ".join(out)

INDICES={'SPX':'SPX_daily_2016_2026.csv','NDX':'NDX_daily_2016_2026.csv',
         'DJ30':'DJ30_daily_2016_2026.csv','GER40':'GER40_daily_2016_2026.csv',
         'UK100':'UK100_daily_2016_2026.csv'}
BASE='/Users/jo/Tick/'

def buyhold(rows):
    return rows[-1][4]/rows[0][4]-1

print("="*120)
print("INDEX DAILY TREND EDGE HUNT  (cost 0.75bp/side, 2016-2026 incl 2022 bear)")
print("="*120)
results=[]
for name,fn in INDICES.items():
    rows=load(BASE+fn)
    bh=buyhold(rows)
    print(f"\n### {name}  buy&hold(full)={bh*100:+.0f}%  rows={len(rows)}")
    # Donchian sweep
    for N in (20,40,55):
        for short in (False,True):
            for ema_n in (None,100):
                tr=run_donchian(rows,N,short,ema_n)
                s=stats(tr)
                if not s: continue
                pf,net,n,wr,by=s
                y2022=by.get(2022,[0,0])
                tag=f"Donch{N}{'+S' if short else 'L'}{'+ema100' if ema_n else ''}"
                results.append((name,tag,pf,net,n,y2022[0]))
                print(f"  {tag:20s} PF={pf:5.2f} net={net*100:+7.0f}% n={n:3d} wr={wr*100:2.0f}% | 2022={y2022[0]*100:+.0f}%/{y2022[1]} | {fmt_year(by)}")
    # Keltner long-only + l/s
    for short in (False,True):
        tr=run_keltner(rows,20,2.0,20,short)
        s=stats(tr)
        if s:
            pf,net,n,wr,by=s; y=by.get(2022,[0,0])
            tag=f"Kelt20/2{'+S' if short else 'L'}"
            results.append((name,tag,pf,net,n,y[0]))
            print(f"  {tag:20s} PF={pf:5.2f} net={net*100:+7.0f}% n={n:3d} wr={wr*100:2.0f}% | 2022={y[0]*100:+.0f}%/{y[1]} | {fmt_year(by)}")
    # MA cross 20/100, 50/200
    for fast,slow in ((20,100),(50,200)):
        for short in (False,True):
            tr=run_macross(rows,fast,slow,short)
            s=stats(tr)
            if s:
                pf,net,n,wr,by=s; y=by.get(2022,[0,0])
                tag=f"MA{fast}/{slow}{'+S' if short else 'L'}"
                results.append((name,tag,pf,net,n,y[0]))
                print(f"  {tag:20s} PF={pf:5.2f} net={net*100:+7.0f}% n={n:3d} wr={wr*100:2.0f}% | 2022={y[0]*100:+.0f}%/{y[1]} | {fmt_year(by)}")

print("\n"+"="*120)
print("TOP CELLS by PF (full-span), filtered n>=8:")
for r in sorted([r for r in results if r[4]>=8], key=lambda x:-x[2])[:25]:
    name,tag,pf,net,n,y22=r
    v = "ROBUST(2022+)" if y22>0 else ("flat2022" if abs(y22)<0.03 else "BULL-BETA(2022-)")
    print(f"  {name:6s} {tag:20s} PF={pf:5.2f} net={net*100:+7.0f}% n={n:3d} 2022={y22*100:+.1f}% -> {v}")
