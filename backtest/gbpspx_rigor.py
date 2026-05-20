#!/usr/bin/env python3
"""Full rigor on GBP-SPX pair candidate."""
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

def run_pair(a, b, win, zin, zout, hold, cost_pct=0.0001, ts_lo=0, ts_hi=10**12, return_trades=False):
    a_idx={x.ts:x for x in a}
    common=[(x.ts, x, a_idx[x.ts]) for x in b if x.ts in a_idx and ts_lo<=x.ts<ts_hi]
    if len(common)<200: return ([], []) if return_trades else []
    spreads=[math.log(c[2].c)-math.log(c[1].c) for c in common]
    pnls=[]; trades_ts=[]; active=False; ea=0; eb=0; is_long=False; ei=0
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
                r=math.log(ca/ea) - math.log(cb/eb)
                if not is_long: r=-r
                pnls.append(r - 2*cost_pct)
                trades_ts.append(common[i][0])
                active=False
            continue
        if z>zin: is_long=False; ea=common[i][2].c; eb=common[i][1].c; active=True; ei=i
        elif z<-zin: is_long=True; ea=common[i][2].c; eb=common[i][1].c; active=True; ei=i
    return (pnls, trades_ts) if return_trades else pnls

def main():
    gbp=load("/Users/jo/Tick/GBPUSD_merged.h1.csv")
    spx=load("/Users/jo/Tick/SPXUSD_merged.h1.csv")
    cfg=dict(win=200, zin=3.0, zout=0.0, hold=6)
    gbp_idx={x.ts for x in gbp}
    common=sorted(x.ts for x in spx if x.ts in gbp_idx)
    ts_lo, ts_hi = common[0], common[-1]

    print(f"GBP h1={len(gbp)} SPX h1={len(spx)} common ts overlap {(ts_hi-ts_lo)/86400:.0f} days")
    print(f"Config: {cfg}")

    # Cost stress
    print("\n=== Cost stress ===")
    for c in [0.00005, 0.00010, 0.00015, 0.00020, 0.00030, 0.00050]:
        p=run_pair(gbp, spx, **cfg, cost_pct=c)
        s=stats(p)
        if s: print(f"  cost={c*10000:5.1f}bps/leg: n={s['n']:<3} PnL={s['pnl']*100:>6.2f}% Sh={s['sharpe']:>5.2f} MDD={s['mdd']*100:.2f}%")

    # Monthly distribution
    print("\n=== Monthly PnL ===")
    p, ts = run_pair(gbp, spx, **cfg, cost_pct=0.0001, return_trades=True)
    by_month = {}
    for pnl, t in zip(p, ts):
        dt = datetime.fromtimestamp(t, tz=timezone.utc)
        key = (dt.year, dt.month)
        by_month.setdefault(key, []).append(pnl)
    pos=0; total=0
    for k in sorted(by_month):
        ps = by_month[k]
        s = sum(ps); w = sum(1 for x in ps if x>0)
        print(f"  {k[0]}-{k[1]:02d}: n={len(ps):<2} pnl={s*100:>6.2f}% wr={100*w/len(ps):>4.1f}%")
        if s>0: pos+=1
        total+=1
    print(f"  Positive: {pos}/{total} ({100*pos/total:.1f}%)")

    # Monte Carlo permutation
    print("\n=== Monte Carlo permutation (n=10000) ===")
    actual_sh = stats(p)["sharpe"]
    rnd = random.Random(42)
    ge = 0
    for _ in range(10000):
        perm = [(x if rnd.random()>0.5 else -x) for x in p]
        s = stats(perm)
        if s["sharpe"] >= actual_sh: ge += 1
    print(f"  Actual Sh={actual_sh:.3f}, perm >= actual: {ge}/10000 (p={ge/10000:.4f})")

    # Robustness ±20% on params
    print("\n=== Param robustness ±20% ===")
    perturbs = [("base", cfg)]
    for k in ["win","zin","hold"]:
        for mult in [0.8, 1.2]:
            cf = dict(cfg); cf[k] = type(cfg[k])(cfg[k]*mult)
            perturbs.append((f"{k}*{mult}", cf))
    cf = dict(cfg); cf["zout"]=0.3; perturbs.append(("zout=0.3", cf))
    cf = dict(cfg); cf["zout"]=0.5; perturbs.append(("zout=0.5", cf))
    for name, cfp in perturbs:
        p2=run_pair(gbp, spx, **cfp, cost_pct=0.0001)
        s2=stats(p2)
        if s2: print(f"  {name:<14}: n={s2['n']:<3} PnL={s2['pnl']*100:>6.2f}% Sh={s2['sharpe']:>5.2f}")

if __name__=="__main__":
    main()
