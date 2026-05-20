#!/usr/bin/env python3
"""IS/OOS rigor on top cross-asset pair candidates."""
import os, csv, math
from collections import deque
from dataclasses import dataclass

@dataclass
class Bar: ts:int=0; o:float=0; h:float=0; l:float=0; c:float=0

def load(p):
    out=[]
    with open(p) as f:
        r=csv.reader(f); next(r,None)
        for row in r: out.append(Bar(int(row[0]),float(row[1]),float(row[2]),float(row[3]),float(row[4])))
    return out

def stats(pnls):
    if not pnls: return None
    n=len(pnls); pnl=sum(pnls); wr=100*sum(1 for p in pnls if p>0)/n
    m=pnl/n; v=sum((p-m)**2 for p in pnls)/max(1,n-1); sd=math.sqrt(v)
    sh=(m/sd*math.sqrt(252)) if sd>0 else 0
    cum=0; pk=0; mdd=0
    for p in pnls:
        cum+=p
        if cum>pk: pk=cum
        if pk-cum>mdd: mdd=pk-cum
    return dict(n=n,pnl=pnl,wr=wr,sharpe=sh,mdd=mdd)

def run_pair(a_bars, b_bars, win, zin, zout, hold, cost_pct=0.0001, ts_lo=0, ts_hi=10**12):
    a_idx={b.ts:b for b in a_bars}
    common=[(b.ts, b, a_idx[b.ts]) for b in b_bars if b.ts in a_idx and ts_lo<=b.ts<ts_hi]
    if len(common)<200: return []
    spreads=[math.log(c[2].c)-math.log(c[1].c) for c in common]
    pnls=[]; active=False; ea=0; eb=0; is_long=False; ei=0
    for i in range(len(spreads)):
        if i<win: continue
        wdat=spreads[i-win:i]
        m=sum(wdat)/win; sd=math.sqrt(sum((x-m)**2 for x in wdat)/(win-1))
        if sd<=0: continue
        z=(spreads[i]-m)/sd
        if active:
            cz=(spreads[i]-m)/sd
            hit=(cz<=zout) if is_long else (cz>=-zout)
            stop=(cz<-3.5) if is_long else (cz>3.5)
            timed=(i-ei)>=hold
            if hit or stop or timed:
                ca=common[i][2].c; cb=common[i][1].c
                r = math.log(ca/ea) - math.log(cb/eb)
                if not is_long: r = -r
                pnls.append(r - 2*cost_pct)
                active=False
            continue
        if z>zin: is_long=False; ea=common[i][2].c; eb=common[i][1].c; active=True; ei=i
        elif z<-zin: is_long=True; ea=common[i][2].c; eb=common[i][1].c; active=True; ei=i
    return pnls

def isoos(a_name, b_name, a, b, cfg, ts_lo, ts_hi):
    mid = ts_lo + (ts_hi - ts_lo) // 2
    is_pnls  = run_pair(a, b, **cfg, ts_lo=ts_lo, ts_hi=mid)
    oos_pnls = run_pair(a, b, **cfg, ts_lo=mid,   ts_hi=ts_hi+1)
    full     = run_pair(a, b, **cfg)
    print(f"\n== {a_name}-{b_name} IS/OOS (cfg={cfg}) ==")
    for tag, p in [("IS ", is_pnls), ("OOS", oos_pnls), ("FUL", full)]:
        s = stats(p)
        if s: print(f"  {tag} n={s['n']:<3} PnL={s['pnl']*100:>6.2f}% WR={s['wr']:>4.1f}% Sh={s['sharpe']:>5.2f} MDD={s['mdd']*100:>5.2f}%")

def wf(a_name, b_name, a, b, cfg, ts_lo, ts_hi, folds=4):
    print(f"\n== {a_name}-{b_name} WF {folds}-fold ==")
    pos = 0
    for f in range(folds):
        lo = ts_lo + (ts_hi-ts_lo)*f//folds
        hi = ts_lo + (ts_hi-ts_lo)*(f+1)//folds
        p = run_pair(a, b, **cfg, ts_lo=lo, ts_hi=hi)
        s = stats(p)
        if s:
            print(f"  F{f+1} n={s['n']:<3} PnL={s['pnl']*100:>6.2f}% Sh={s['sharpe']:>5.2f} MDD={s['mdd']*100:>5.2f}%")
            if s["sharpe"]>0: pos+=1
        else:
            print(f"  F{f+1} no trades")
    print(f"  Positive folds: {pos}/{folds}")

def main():
    bars = {
        "EUR": load("/Users/jo/Tick/EURUSD_merged.h1.csv"),
        "GBP": load("/Users/jo/Tick/GBPUSD_merged.h1.csv"),
        "JPY": load("/Users/jo/Tick/USDJPY_merged.h1.csv"),
        "SPX": load("/Users/jo/Tick/SPXUSD_merged.h1.csv"),
        "GER": load("/Users/jo/Tick/GER40_merged.h1.csv"),
    }
    # Common ts range overlap (use union of intersections per pair)
    bars["AUD"] = load("/Users/jo/Tick/AUDUSD_merged.h1.csv")
    bars["NZD"] = load("/Users/jo/Tick/NZDUSD_merged.h1.csv")
    bars["CAD"] = load("/Users/jo/Tick/USDCAD_merged.h1.csv")
    bars["NSX"] = load("/Users/jo/Tick/NSXUSD_merged.h1.csv")
    for a,b,cfg in [
        ("CAD","AUD", dict(win=20,  zin=2.5, zout=0.5, hold=24)),
        ("CAD","NZD", dict(win=20,  zin=3.0, zout=0.5, hold=24)),
        ("AUD","NSX", dict(win=200, zin=3.0, zout=0.3, hold=6)),
        ("AUD","SPX", dict(win=200, zin=3.0, zout=0.0, hold=6)),
        ("NZD","SPX", dict(win=40,  zin=3.0, zout=0.0, hold=6)),
    ]:
        # Compute ts overlap
        a_idx={x.ts for x in bars[a]}
        common_ts = sorted(x.ts for x in bars[b] if x.ts in a_idx)
        if not common_ts: continue
        ts_lo, ts_hi = common_ts[0], common_ts[-1]
        isoos(a,b, bars[a], bars[b], cfg, ts_lo, ts_hi)
        wf(a,b, bars[a], bars[b], cfg, ts_lo, ts_hi, folds=4)

if __name__=="__main__":
    main()
