#!/usr/bin/env python3
"""USDCAD vs Brent Crude Oil pairs probe + rigor.
CAD is petro-currency; USDCAD inversely correlated with oil price.
Spread = log(USDCAD) + log(BCOUSD) should be relatively stable (cointegrated).
Or use log(USDCAD) - log(BCOUSD) - whichever shows mean reversion.
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

def run_pair(a, b, win, zin, zout, hold, cost_pct=0.0001, ts_lo=0, ts_hi=10**12,
             return_trades=False, sign=+1):
    """Spread = sign*log(a) - log(b). sign=+1 normal, sign=-1 to test sum spread."""
    a_idx={x.ts:x for x in a}
    common=[(x.ts, x, a_idx[x.ts]) for x in b if x.ts in a_idx and ts_lo<=x.ts<ts_hi]
    if len(common)<200: return ([],[]) if return_trades else []
    spreads=[sign*math.log(c[2].c) - math.log(c[1].c) if sign>0 else math.log(c[2].c) + math.log(c[1].c) for c in common]
    # If sign=-1: use sum spread (inverse correlation - test cointegrating in sum)
    if sign<0:
        spreads=[math.log(c[2].c) + math.log(c[1].c) for c in common]
    pnls=[]; ts_list=[]; active=False; ea=0; eb=0; is_long=False; ei=0
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
                if sign>0:
                    r=math.log(ca/ea)-math.log(cb/eb)
                else:
                    # Sum-spread trade: both legs same direction
                    r=math.log(ca/ea)+math.log(cb/eb)
                if not is_long: r=-r
                pnls.append(r - 2*cost_pct)
                ts_list.append(common[i][0])
                active=False
            continue
        if z>zin: is_long=False; ea=common[i][2].c; eb=common[i][1].c; active=True; ei=i
        elif z<-zin: is_long=True; ea=common[i][2].c; eb=common[i][1].c; active=True; ei=i
    return (pnls, ts_list) if return_trades else pnls

def main():
    cad = load("/Users/jo/Tick/USDCAD_merged.h1.csv")
    bco = load("/Users/jo/Tick/BCOUSD_merged.h1.csv")
    print(f"USDCAD h1={len(cad)} BCOUSD h1={len(bco)}")

    print("\n=== H1 USDCAD vs BCOUSD: log-diff spread ===")
    print(f"{'w':>3} {'zi':>4} {'zo':>4} {'h':>3}  {'n':>4} {'PnL%':>7} {'WR':>5} {'Sh':>6}")
    print('-'*60)
    best_diff=None
    for w in [20, 40, 60, 120, 200]:
        for zi in [1.5, 2.0, 2.5, 3.0]:
            for zo in [0.0, 0.3, 0.5]:
                for ho in [6, 12, 24, 48]:
                    t=run_pair(cad, bco, w, zi, zo, ho, sign=+1)
                    s=stats(t)
                    if not s or s["n"]<20: continue
                    if s["sharpe"]>1.5:
                        tag=" ***" if s["sharpe"]>3.0 else " *"
                        print(f"{w:>3} {zi:>4} {zo:>4} {ho:>3}  {s['n']:>4} {s['pnl']*100:>6.2f}% {s['wr']:>4.1f}% {s['sharpe']:>5.2f}{tag}")
                    if best_diff is None or s["sharpe"]>best_diff[1]["sharpe"]:
                        best_diff = ((w,zi,zo,ho), s)
    if best_diff:
        (w,zi,zo,h), s = best_diff
        print(f"BEST diff: w={w} zi={zi} zo={zo} h={h}: n={s['n']} Sh={s['sharpe']:.2f} PnL={s['pnl']*100:.2f}%")

    print("\n=== H1 USDCAD vs BCOUSD: log-SUM spread (inverse correlation) ===")
    best_sum=None
    for w in [20, 40, 60, 120, 200]:
        for zi in [1.5, 2.0, 2.5, 3.0]:
            for zo in [0.0, 0.3, 0.5]:
                for ho in [6, 12, 24, 48]:
                    t=run_pair(cad, bco, w, zi, zo, ho, sign=-1)
                    s=stats(t)
                    if not s or s["n"]<20: continue
                    if s["sharpe"]>1.5:
                        tag=" ***" if s["sharpe"]>3.0 else " *"
                        print(f"{w:>3} {zi:>4} {zo:>4} {ho:>3}  {s['n']:>4} {s['pnl']*100:>6.2f}% {s['wr']:>4.1f}% {s['sharpe']:>5.2f}{tag}")
                    if best_sum is None or s["sharpe"]>best_sum[1]["sharpe"]:
                        best_sum = ((w,zi,zo,ho), s)
    if best_sum:
        (w,zi,zo,h), s = best_sum
        print(f"BEST sum: w={w} zi={zi} zo={zo} h={h}: n={s['n']} Sh={s['sharpe']:.2f} PnL={s['pnl']*100:.2f}%")

    # Correlation check
    cad_idx={x.ts:x for x in cad}
    common=[(x.ts, x.c, cad_idx[x.ts].c) for x in bco if x.ts in cad_idx]
    bco_rets=[]; cad_rets=[]
    for i in range(1, len(common)):
        bco_rets.append(math.log(common[i][1]/common[i-1][1]))
        cad_rets.append(math.log(common[i][2]/common[i-1][2]))
    n=len(bco_rets)
    mb=sum(bco_rets)/n; mc=sum(cad_rets)/n
    cov=sum((bco_rets[i]-mb)*(cad_rets[i]-mc) for i in range(n))/(n-1)
    vb=sum((x-mb)**2 for x in bco_rets)/(n-1)
    vc=sum((x-mc)**2 for x in cad_rets)/(n-1)
    corr=cov/(math.sqrt(vb)*math.sqrt(vc)) if vb>0 and vc>0 else 0
    print(f"\n  BCO<->USDCAD H1 log-return Pearson: {corr:.4f}")
    print(f"  (expected NEGATIVE: oil up -> CAD strengthens -> USDCAD down)")

    # Pick best, do IS/OOS + cost stress
    candidates = []
    if best_diff: candidates.append(("DIFF", best_diff, +1))
    if best_sum:  candidates.append(("SUM",  best_sum,  -1))
    candidates.sort(key=lambda x: -x[1][1]["sharpe"])
    name, ((w,zi,zo,h), s), sign = candidates[0]
    cfg=dict(win=w, zin=zi, zout=zo, hold=h, sign=sign)
    print(f"\n=== Best ({name}) full rigor ===")
    print(f"Config: {cfg}")

    cad_idx_ts={x.ts for x in cad}
    common_ts=sorted(x.ts for x in bco if x.ts in cad_idx_ts)
    ts_lo, ts_hi = common_ts[0], common_ts[-1]
    mid = ts_lo + (ts_hi-ts_lo)//2
    is_p = run_pair(cad, bco, ts_lo=ts_lo, ts_hi=mid, cost_pct=0.0001, **cfg)
    oos_p = run_pair(cad, bco, ts_lo=mid, ts_hi=ts_hi+1, cost_pct=0.0001, **cfg)
    full_p = run_pair(cad, bco, cost_pct=0.0001, **cfg)
    print("\n-- IS/OOS --")
    for tag, p in [("IS ",is_p),("OOS",oos_p),("FUL",full_p)]:
        s=stats(p)
        if s: print(f"  {tag} n={s['n']:<3} PnL={s['pnl']*100:>6.2f}% WR={s['wr']:>4.1f}% Sh={s['sharpe']:>5.2f}")

    print("\n-- WF 4-fold --")
    pos=0
    for f in range(4):
        lo=ts_lo+(ts_hi-ts_lo)*f//4
        hi=ts_lo+(ts_hi-ts_lo)*(f+1)//4
        p=run_pair(cad, bco, ts_lo=lo, ts_hi=hi, cost_pct=0.0001, **cfg)
        s=stats(p)
        if s:
            print(f"  F{f+1} n={s['n']:<3} PnL={s['pnl']*100:>6.2f}% Sh={s['sharpe']:>5.2f}")
            if s["sharpe"]>0: pos+=1
    print(f"  Positive: {pos}/4")

    print("\n-- Cost stress --")
    for c in [0.00005, 0.00010, 0.00015, 0.00020, 0.00030, 0.00050]:
        p=run_pair(cad, bco, cost_pct=c, **cfg)
        s=stats(p)
        if s: print(f"  cost={c*10000:.1f}bps: n={s['n']:<3} PnL={s['pnl']*100:>6.2f}% Sh={s['sharpe']:>5.2f}")

    print("\n-- Robustness +/-20% --")
    perturbs=[("base",cfg)]
    for k in ["win","zin","hold"]:
        for mult in [0.8,1.2]:
            cf=dict(cfg); cf[k]=type(cfg[k])(cfg[k]*mult)
            perturbs.append((f"{k}*{mult}",cf))
    for name2,cfp in perturbs:
        p2=run_pair(cad, bco, cost_pct=0.0001, **cfp)
        s2=stats(p2)
        if s2: print(f"  {name2:<10}: n={s2['n']:<3} Sh={s2['sharpe']:>5.2f}")

    print("\n-- Monte Carlo n=10000 --")
    actual=stats(full_p)["sharpe"]
    rnd=random.Random(42); ge=0
    for _ in range(10000):
        perm=[(x if rnd.random()>0.5 else -x) for x in full_p]
        s=stats(perm)
        if s["sharpe"]>=actual: ge+=1
    print(f"  Actual Sh={actual:.3f}, ge={ge}/10000, p={ge/10000:.4f}")

    print("\n-- Monthly --")
    p, ts = run_pair(cad, bco, cost_pct=0.0001, return_trades=True, **cfg)
    by_month={}
    for pnl,t in zip(p,ts):
        dt=datetime.fromtimestamp(t,tz=timezone.utc)
        key=(dt.year,dt.month)
        by_month.setdefault(key,[]).append(pnl)
    pos=0; total=0
    for k in sorted(by_month):
        ps=by_month[k]; s=sum(ps); w=sum(1 for x in ps if x>0)
        print(f"  {k[0]}-{k[1]:02d}: n={len(ps):<3} pnl={s*100:>6.2f}% wr={100*w/len(ps):>4.1f}%")
        if s>0: pos+=1; total+=1
        else: total+=1
    print(f"  Positive: {pos}/{total} ({100*pos/total:.1f}%)")

if __name__=="__main__":
    main()
