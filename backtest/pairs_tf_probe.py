#!/usr/bin/env python3
"""Pairs at different timeframes -- H1 vs H4 vs D1."""
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

def pairs(a_bars, b_bars, win, zin, zout, hold, cost_per_leg=0.00015):
    a_idx={b.ts:b for b in a_bars}
    common=[(b.ts, b, a_idx[b.ts]) for b in b_bars if b.ts in a_idx]
    if len(common)<200: return []
    # log spread (more stable)
    spreads=[math.log(c[2].c) - math.log(c[1].c) for c in common]
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
                # 0.01 lot each, $1000/unit each leg (both EUR-like majors)
                a_pnl=(1 if is_long else -1)*(ca-ea)*1000*0.01
                b_pnl=(-1 if is_long else 1)*(cb-eb)*1000*0.01
                cost=cost_per_leg*1000*0.01*2
                pnls.append(a_pnl+b_pnl-cost)
                active=False
            continue
        if z>zin: is_long=False; es=spreads[i]; ea=common[i][2].c; eb=common[i][1].c; active=True; ei=i
        elif z<-zin: is_long=True; es=spreads[i]; ea=common[i][2].c; eb=common[i][1].c; active=True; ei=i
    return pnls

def main():
    eur_h1=load("/Users/jo/Tick/EURUSD_merged.h1.csv")
    gbp_h1=load("/Users/jo/Tick/GBPUSD_merged.h1.csv")
    jpy_h1=load("/Users/jo/Tick/USDJPY_merged.h1.csv")
    eur_h4=load("/Users/jo/Tick/EURUSD_merged.h4.csv")
    gbp_h4=load("/Users/jo/Tick/GBPUSD_merged.h4.csv")
    jpy_h4=load("/Users/jo/Tick/USDJPY_merged.h4.csv")
    print(f"EUR h1={len(eur_h1)} h4={len(eur_h4)} GBP h1={len(gbp_h1)} h4={len(gbp_h4)} JPY h1={len(jpy_h1)} h4={len(jpy_h4)}")

    print("\n=== H1 ===")
    for w in [60, 120]:
        for zi in [1.5, 2.0]:
            for ho in [24, 48]:
                t=pairs(eur_h1, gbp_h1, w, zi, 0.5, ho)
                s=stats(t)
                if s and s["n"]>=10:
                    print(f"  w={w} zi={zi} h={ho}: n={s['n']:<4} PnL=${s['pnl']:>7.2f} Sh={s['sharpe']:>5.2f} MDD=${s['mdd']:>6.2f}")

    print("\n=== H4 EUR-GBP ===")
    for w in [20, 40, 60]:
        for zi in [1.5, 2.0]:
            for ho in [6, 12, 24]:
                t=pairs(eur_h4, gbp_h4, w, zi, 0.5, ho)
                s=stats(t)
                if s and s["n"]>=10:
                    print(f"  w={w} zi={zi} h={ho}: n={s['n']:<4} PnL=${s['pnl']:>7.2f} Sh={s['sharpe']:>5.2f} MDD=${s['mdd']:>6.2f}")

    print("\n=== H1 EUR-JPY (cross via /USD) ===")
    # log(EURUSD) - log(USDJPY) ~ log(EURUSD * JPYUSD) ~ log(EURJPY) inverse?
    # Use raw log-spread of mid prices -- structure may not mean revert though
    for w in [60, 120]:
        for zi in [1.5, 2.0]:
            for ho in [24, 48]:
                t=pairs(eur_h1, jpy_h1, w, zi, 0.5, ho, cost_per_leg=0.00015)
                s=stats(t)
                if s and s["n"]>=10:
                    tag=" *" if s["sharpe"]>1.5 else ""
                    print(f"  w={w} zi={zi} h={ho}: n={s['n']:<4} PnL=${s['pnl']:>7.2f} Sh={s['sharpe']:>5.2f} MDD=${s['mdd']:>6.2f}{tag}")

    print("\n=== H1 GBP-JPY (cross) ===")
    for w in [60, 120]:
        for zi in [1.5, 2.0]:
            for ho in [24, 48]:
                t=pairs(gbp_h1, jpy_h1, w, zi, 0.5, ho, cost_per_leg=0.00015)
                s=stats(t)
                if s and s["n"]>=10:
                    tag=" *" if s["sharpe"]>1.5 else ""
                    print(f"  w={w} zi={zi} h={ho}: n={s['n']:<4} PnL=${s['pnl']:>7.2f} Sh={s['sharpe']:>5.2f} MDD=${s['mdd']:>6.2f}{tag}")

if __name__=="__main__":
    main()
