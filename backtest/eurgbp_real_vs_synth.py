#!/usr/bin/env python3
"""
Compare real EURGBP vs synthetic (EURUSD/GBPUSD). Validate single-instrument
mean-reversion edge holds on REAL EURGBP bars. Full rigor.
"""
import os, csv, math, random
from collections import deque
from dataclasses import dataclass
from datetime import datetime, timezone

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

def build_synth(eur, gbp):
    eur_idx={b.ts:b for b in eur}
    out=[]
    for b in gbp:
        if b.ts in eur_idx:
            e=eur_idx[b.ts]
            c=e.c/b.c
            o=e.o/b.o
            h=max(e.h/b.l, e.l/b.h, c, o)
            l=min(e.l/b.h, e.h/b.l, c, o)
            out.append(Bar(b.ts,o,h,l,c))
    return out

def run_meanrev(bars, win, zin, zout, hold, cost_pct=0.0001, ts_lo=0, ts_hi=10**12, return_ts=False):
    pnls=[]; ts_list=[]
    active=False; entry=0; is_long=False; entry_idx=0
    closes=[b.c for b in bars]
    for i,b in enumerate(bars):
        if not (ts_lo<=b.ts<ts_hi): continue
        if i<win: continue
        wd=closes[i-win:i]
        m=sum(wd)/win; v=sum((x-m)**2 for x in wd)/(win-1); sd=math.sqrt(v)
        if sd<=0: continue
        z=(b.c-m)/sd
        if active:
            cz=(b.c-m)/sd
            hit=(cz<=zout) if is_long else (cz>=-zout)
            stop=(cz<-3.5) if is_long else (cz>3.5)
            timed=(i-entry_idx)>=hold
            if hit or stop or timed:
                r=math.log(b.c/entry)
                if not is_long: r=-r
                pnls.append(r - cost_pct)
                ts_list.append(b.ts)
                active=False
            continue
        if z>zin: is_long=False; entry=b.c; active=True; entry_idx=i
        elif z<-zin: is_long=True; entry=b.c; active=True; entry_idx=i
    return (pnls, ts_list) if return_ts else pnls

def main():
    eur = load("/Users/jo/Tick/EURUSD_merged.h1.csv")
    gbp = load("/Users/jo/Tick/GBPUSD_merged.h1.csv")
    eurgbp = load("/Users/jo/Tick/EURGBP_merged.h1.csv")
    synth = build_synth(eur, gbp)
    print(f"Real EURGBP h1={len(eurgbp)}, synthetic EUR/GBP h1={len(synth)}")

    # Correlation check
    synth_idx = {b.ts: b.c for b in synth}
    common = [(b.ts, b.c, synth_idx[b.ts]) for b in eurgbp if b.ts in synth_idx]
    print(f"Aligned bars: {len(common)}")
    if common:
        real_v = [c[1] for c in common]
        syn_v  = [c[2] for c in common]
        n=len(real_v)
        mr=sum(real_v)/n; ms=sum(syn_v)/n
        cov=sum((real_v[i]-mr)*(syn_v[i]-ms) for i in range(n))/(n-1)
        vr=sum((x-mr)**2 for x in real_v)/(n-1)
        vs=sum((x-ms)**2 for x in syn_v)/(n-1)
        corr=cov/(math.sqrt(vr)*math.sqrt(vs))
        # Mean absolute deviation
        mad = sum(abs(real_v[i]-syn_v[i]) for i in range(n))/n
        print(f"Real vs synth: Pearson={corr:.6f}, mean abs diff={mad:.6f} ({mad/mr*10000:.1f} bps)")

    # Best config from synthetic test
    cfg = dict(win=120, zin=3.0, zout=0.5, hold=12)
    print(f"\nConfig: {cfg}\n")

    # Sweep on REAL EURGBP - full grid
    print("=== REAL EURGBP grid (top configs) ===")
    print(f"{'w':>3} {'zi':>4} {'zo':>4} {'h':>3}  {'n':>4} {'PnL%':>7} {'WR':>5} {'Sh':>6}")
    print('-'*60)
    best=None
    for w in [40, 60, 80, 120, 200]:
        for zi in [1.0, 1.5, 2.0, 2.5, 3.0]:
            for zo in [0.0, 0.3, 0.5]:
                for ho in [6, 12, 24, 48]:
                    p = run_meanrev(eurgbp, w, zi, zo, ho, cost_pct=0.0001)
                    s = stats(p)
                    if not s or s["n"]<20: continue
                    if s["sharpe"]>3.5:
                        print(f"{w:>3} {zi:>4} {zo:>4} {ho:>3}  {s['n']:>4} {s['pnl']*100:>6.2f}% {s['wr']:>4.1f}% {s['sharpe']:>5.2f}")
                    if best is None or s["sharpe"]>best[1]["sharpe"]:
                        best = ((w,zi,zo,ho), s)
    if best:
        (w,zi,zo,h),s = best
        print(f"\nBEST REAL: w={w} zi={zi} zo={zo} h={h}: n={s['n']} PnL={s['pnl']*100:.2f}% Sh={s['sharpe']:.2f}")
        best_cfg = dict(win=w, zin=zi, zout=zo, hold=h)

    # Pick best for full rigor
    cfg = best_cfg

    print(f"\n=== Full rigor on REAL EURGBP with cfg={cfg} ===")
    ts_lo=eurgbp[0].ts; ts_hi=eurgbp[-1].ts; mid=ts_lo+(ts_hi-ts_lo)//2
    is_p = run_meanrev(eurgbp, **cfg, cost_pct=0.0001, ts_lo=ts_lo, ts_hi=mid)
    oos_p = run_meanrev(eurgbp, **cfg, cost_pct=0.0001, ts_lo=mid, ts_hi=ts_hi+1)
    full_p = run_meanrev(eurgbp, **cfg, cost_pct=0.0001)
    print("\n-- IS/OOS --")
    for tag,p in [("IS ",is_p),("OOS",oos_p),("FUL",full_p)]:
        s=stats(p)
        if s: print(f"  {tag} n={s['n']:<3} PnL={s['pnl']*100:>6.2f}% WR={s['wr']:>4.1f}% Sh={s['sharpe']:>5.2f} MDD={s['mdd']*100:.2f}%")

    print("\n-- WF 4-fold --")
    pos=0
    for f in range(4):
        lo=ts_lo+(ts_hi-ts_lo)*f//4
        hi=ts_lo+(ts_hi-ts_lo)*(f+1)//4
        p=run_meanrev(eurgbp, **cfg, cost_pct=0.0001, ts_lo=lo, ts_hi=hi)
        s=stats(p)
        if s:
            print(f"  F{f+1} n={s['n']:<3} PnL={s['pnl']*100:>6.2f}% Sh={s['sharpe']:>5.2f}")
            if s["sharpe"]>0: pos+=1
    print(f"  Positive: {pos}/4")

    print("\n-- Cost stress --")
    for c in [0.00005,0.00010,0.00015,0.00020,0.00030,0.00050]:
        p=run_meanrev(eurgbp, **cfg, cost_pct=c)
        s=stats(p)
        if s: print(f"  cost={c*10000:.1f}bps: n={s['n']:<3} PnL={s['pnl']*100:>6.2f}% Sh={s['sharpe']:>5.2f}")

    print("\n-- Robustness +/-20% --")
    perturbs=[("base",cfg)]
    for k in ["win","zin","hold"]:
        for mult in [0.8,1.2]:
            cf=dict(cfg); cf[k]=type(cfg[k])(cfg[k]*mult)
            perturbs.append((f"{k}*{mult}",cf))
    for name,cfp in perturbs:
        p2=run_meanrev(eurgbp, **cfp, cost_pct=0.0001)
        s2=stats(p2)
        if s2: print(f"  {name:<10}: n={s2['n']:<3} Sh={s2['sharpe']:>5.2f}")

    print("\n-- Monte Carlo n=10000 --")
    actual=stats(full_p)["sharpe"]
    rnd=random.Random(42); ge=0
    for _ in range(10000):
        perm=[(x if rnd.random()>0.5 else -x) for x in full_p]
        s=stats(perm)
        if s["sharpe"]>=actual: ge+=1
    print(f"  Actual Sh={actual:.3f}, ge={ge}/10000 p={ge/10000:.4f}")

    print("\n-- Monthly --")
    p, ts = run_meanrev(eurgbp, **cfg, cost_pct=0.0001, return_ts=True)
    by_month={}
    for pnl,t in zip(p,ts):
        dt=datetime.fromtimestamp(t,tz=timezone.utc)
        key=(dt.year,dt.month)
        by_month.setdefault(key,[]).append(pnl)
    pos=0; total=0
    for k in sorted(by_month):
        ps=by_month[k]; s=sum(ps); w=sum(1 for x in ps if x>0)
        print(f"  {k[0]}-{k[1]:02d}: n={len(ps):<3} pnl={s*100:>6.2f}% wr={100*w/len(ps):>4.1f}%")
        if s>0: pos+=1
        total+=1
    print(f"  Positive: {pos}/{total} ({100*pos/total:.1f}%)")

if __name__=="__main__":
    main()
