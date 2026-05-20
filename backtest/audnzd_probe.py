#!/usr/bin/env python3
"""AUD-NZD pairs probe on 4mo data. Same archetype as EUR-GBP."""
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

def pairs(a_bars, b_bars, win, zin, zout, hold, cost_per_leg=0.00010):
    a_idx={b.ts:b for b in a_bars}
    common=[(b.ts, b, a_idx[b.ts]) for b in b_bars if b.ts in a_idx]
    if len(common)<200: return [], 0
    spreads=[math.log(c[2].c)-math.log(c[1].c) for c in common]
    pnls=[]; active=False; es=0; ea=0; eb=0; is_long=False; ei=0
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
                a_pnl=(1 if is_long else -1)*(ca-ea)*1000*0.01
                b_pnl=(-1 if is_long else 1)*(cb-eb)*1000*0.01
                cost=cost_per_leg*1000*0.01*2
                pnls.append(a_pnl+b_pnl-cost)
                active=False
            continue
        if z>zin: is_long=False; es=spreads[i]; ea=common[i][2].c; eb=common[i][1].c; active=True; ei=i
        elif z<-zin: is_long=True; es=spreads[i]; ea=common[i][2].c; eb=common[i][1].c; active=True; ei=i
    return pnls, len(common)

def pairs_momentum(a_bars, b_bars, win, zin, zout, hold, cost_per_leg=0.00010):
    """Inverse of pairs(): chase spread divergence instead of fade."""
    a_idx={b.ts:b for b in a_bars}
    common=[(b.ts, b, a_idx[b.ts]) for b in b_bars if b.ts in a_idx]
    if len(common)<200: return [], 0
    spreads=[math.log(c[2].c)-math.log(c[1].c) for c in common]
    pnls=[]; active=False; es=0; ea=0; eb=0; is_long=False; ei=0
    for i in range(len(spreads)):
        if i<win: continue
        wdat=spreads[i-win:i]
        m=sum(wdat)/win; sd=math.sqrt(sum((x-m)**2 for x in wdat)/(win-1))
        if sd<=0: continue
        z=(spreads[i]-m)/sd
        if active:
            cz=(spreads[i]-m)/sd
            # exit when z reverts (the opposite of fade)
            hit=(cz<=zin*0.5) if is_long else (cz>=-zin*0.5)
            timed=(i-ei)>=hold
            if hit or timed:
                ca=common[i][2].c; cb=common[i][1].c
                a_pnl=(1 if is_long else -1)*(ca-ea)*1000*0.01
                b_pnl=(-1 if is_long else 1)*(cb-eb)*1000*0.01
                cost=cost_per_leg*1000*0.01*2
                pnls.append(a_pnl+b_pnl-cost)
                active=False
            continue
        # momentum: enter SAME direction as divergence
        if z>zin: is_long=True; es=spreads[i]; ea=common[i][2].c; eb=common[i][1].c; active=True; ei=i
        elif z<-zin: is_long=False; es=spreads[i]; ea=common[i][2].c; eb=common[i][1].c; active=True; ei=i
    return pnls, len(common)

def main():
    aud_h1=load("/Users/jo/Tick/AUDUSD_merged.h1.csv")
    nzd_h1=load("/Users/jo/Tick/NZDUSD_merged.h1.csv")
    aud_h4=load("/Users/jo/Tick/AUDUSD_merged.h4.csv")
    nzd_h4=load("/Users/jo/Tick/NZDUSD_merged.h4.csv")
    print(f"AUD h1={len(aud_h1)} h4={len(aud_h4)} NZD h1={len(nzd_h1)} h4={len(nzd_h4)}")

    print("\n=== H1 AUD-NZD sweep ===")
    for w in [40, 60, 120]:
        for zi in [1.5, 2.0]:
            for ho in [12, 24, 48]:
                t,nc=pairs(aud_h1, nzd_h1, w, zi, 0.5, ho)
                s=stats(t)
                if not s or s["n"]<10: continue
                tag=" ***" if s["sharpe"]>2.0 else (" *" if s["sharpe"]>1.0 else "")
                print(f"  w={w:<3} zi={zi} h={ho:<2}: n={s['n']:<4} PnL=${s['pnl']:>7.2f} WR={s['wr']:>4.1f}% Sh={s['sharpe']:>5.2f} MDD=${s['mdd']:>6.2f}{tag}")

    print("\n=== H4 AUD-NZD sweep ===")
    for w in [20, 40, 60]:
        for zi in [1.5, 2.0]:
            for ho in [6, 12, 24]:
                t,nc=pairs(aud_h4, nzd_h4, w, zi, 0.5, ho)
                s=stats(t)
                if not s or s["n"]<10: continue
                tag=" ***" if s["sharpe"]>2.0 else (" *" if s["sharpe"]>1.0 else "")
                print(f"  w={w:<3} zi={zi} h={ho:<2}: n={s['n']:<4} PnL=${s['pnl']:>7.2f} WR={s['wr']:>4.1f}% Sh={s['sharpe']:>5.2f} MDD=${s['mdd']:>6.2f}{tag}")

    # Also test cost stress on best config
    print("\n=== Cost stress (H1 w=60 zi=2.0 h=24) ===")
    for cost in [0.00005, 0.00010, 0.00015, 0.00020, 0.00030]:
        t,_=pairs(aud_h1, nzd_h1, 60, 2.0, 0.5, 24, cost_per_leg=cost)
        s=stats(t)
        if s: print(f"  cost={cost*10000:.1f}pip: n={s['n']:<3} PnL=${s['pnl']:>6.2f} Sh={s['sharpe']:>5.2f} MDD=${s['mdd']:>5.2f}")

    print("\n=== H1 INVERTED (momentum) ===")
    for w in [40, 60, 120]:
        for zi in [1.5, 2.0, 2.5]:
            for ho in [12, 24, 48]:
                t,_=pairs_momentum(aud_h1, nzd_h1, w, zi, 0.5, ho)
                s=stats(t)
                if not s or s["n"]<10: continue
                tag=" ***" if s["sharpe"]>2.0 else (" *" if s["sharpe"]>1.0 else "")
                print(f"  w={w:<3} zi={zi} h={ho:<2}: n={s['n']:<4} PnL=${s['pnl']:>7.2f} WR={s['wr']:>4.1f}% Sh={s['sharpe']:>5.2f}{tag}")

    print("\n=== Correlation check AUD<->NZD ===")
    # Pearson of H1 returns
    aud_idx={b.ts:b for b in aud_h1}
    common=[(b.ts, b.c, aud_idx[b.ts].c) for b in nzd_h1 if b.ts in aud_idx]
    aud_rets=[]; nzd_rets=[]
    for i in range(1, len(common)):
        nzd_rets.append(math.log(common[i][1]/common[i-1][1]))
        aud_rets.append(math.log(common[i][2]/common[i-1][2]))
    n=len(aud_rets); ma=sum(aud_rets)/n; mn=sum(nzd_rets)/n
    cov=sum((aud_rets[i]-ma)*(nzd_rets[i]-mn) for i in range(n))/(n-1)
    va=sum((x-ma)**2 for x in aud_rets)/(n-1)
    vn=sum((x-mn)**2 for x in nzd_rets)/(n-1)
    corr = cov/(math.sqrt(va)*math.sqrt(vn)) if va>0 and vn>0 else 0
    print(f"  AUD<->NZD H1 log-return Pearson: {corr:.4f}")

if __name__=="__main__":
    main()
