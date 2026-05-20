#!/usr/bin/env python3
"""
EUR-GBP pairs trading rigor: IS/OOS split + cost stress + walk-forward.
"""
import os, csv, math
from collections import deque
from dataclasses import dataclass

@dataclass
class Bar: ts:int=0; o:float=0; h:float=0; l:float=0; c:float=0

def load(path):
    out=[]
    with open(path) as f:
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

def pairs_with_log(eur_h1, gbp_h1, win=60, z_in=2.0, z_out=0.5, hold=24,
                   cost_per_leg=0.00010, ts_filter=None):
    eur_idx={b.ts:b for b in eur_h1}
    common=[(b.ts, b, eur_idx[b.ts]) for b in gbp_h1 if b.ts in eur_idx]
    if ts_filter:
        lo,hi = ts_filter
        common = [c for c in common if lo<=c[0]<hi]
    spreads=[c[2].c - c[1].c for c in common]
    ts_list =[c[0] for c in common]
    pnls=[]; trade_ts=[]
    active=False; entry_spread=0; is_long=False; entry_idx=0
    for i in range(len(spreads)):
        if i<win: continue
        win_data=spreads[i-win:i]
        m=sum(win_data)/win
        sd=math.sqrt(sum((x-m)**2 for x in win_data)/(win-1))
        if sd<=0: continue
        z=(spreads[i]-m)/sd
        if active:
            cur_z=(spreads[i]-m)/sd
            hit_out=(cur_z<=z_out) if is_long else (cur_z>=-z_out)
            stopped=(cur_z<-3.5) if is_long else (cur_z>3.5)
            timed=(i-entry_idx)>=hold
            if hit_out or stopped or timed:
                pnl_spread=(spreads[i]-entry_spread) if is_long else (entry_spread-spreads[i])
                # 2 legs at 0.01 lot, each pair $1000/unit. Spread move directly = $ at 0.01 lot
                # but we trade BOTH so spread move = combined pnl per unit * 0.01
                # If EUR moves +0.0010 and GBP moves -0.0005, spread = +0.0015, both contribute
                # Simplification: spread $ value at 0.01 lot per leg = spread_move * 1000 * 0.01 = $10 per 0.001 unit
                # 0.001 unit of spread = $10 with 0.01 lot per side -> use 10000 multiplier
                pnls.append(pnl_spread * 10000 - cost_per_leg * 2 * 10000)
                trade_ts.append(ts_list[i])
                active=False
            continue
        if z>z_in: is_long=False; entry_spread=spreads[i]; active=True; entry_idx=i
        elif z<-z_in: is_long=True; entry_spread=spreads[i]; active=True; entry_idx=i
    return pnls, trade_ts

def main():
    eur=load("/Users/jo/Tick/EURUSD_merged.h1.csv")
    gbp=load("/Users/jo/Tick/GBPUSD_merged.h1.csv")
    print(f"EUR h1={len(eur)} GBP h1={len(gbp)}")
    eur_idx={b.ts:b for b in eur}
    common_ts=sorted(b.ts for b in gbp if b.ts in eur_idx)
    if not common_ts: print("NO COMMON TS"); return
    ts0=common_ts[0]; tsN=common_ts[-1]
    span=tsN-ts0
    mid=ts0 + span//2
    print(f"Data span: {ts0} -> {tsN} ({span/86400:.0f} days)")
    print(f"IS: {ts0} -> {mid}, OOS: {mid} -> {tsN}")

    print("\n== IS/OOS split (50/50 by time) ==")
    print(f"{'cfg':<40} {'IS_n':>5} {'IS_pnl':>9} {'IS_sh':>6} {'IS_mdd':>7}  {'OOS_n':>5} {'OOS_pnl':>9} {'OOS_sh':>6} {'OOS_mdd':>7}")
    for w in [40, 60, 80, 120]:
        for z in [1.5, 2.0, 2.5]:
            for ho in [12, 24, 48]:
                pi,_=pairs_with_log(eur,gbp,win=w,z_in=z,hold=ho,ts_filter=(ts0,mid))
                po,_=pairs_with_log(eur,gbp,win=w,z_in=z,hold=ho,ts_filter=(mid,tsN+1))
                si=stats(pi); so=stats(po)
                if not si or not so: continue
                if si["sharpe"]<0.5 or so["sharpe"]<0.5: continue
                cfg=f"w={w}_z={z}_h={ho}"
                print(f"  {cfg:<40} {si['n']:>5} ${si['pnl']:>7.2f} {si['sharpe']:>5.2f} ${si['mdd']:>5.2f}  {so['n']:>5} ${so['pnl']:>7.2f} {so['sharpe']:>5.2f} ${so['mdd']:>5.2f}")

    print("\n== Cost stress (full data, w=60 z=2.0 h=24) ==")
    for cost in [0.00005, 0.00010, 0.00015, 0.00020]:
        p,_=pairs_with_log(eur,gbp,win=60,z_in=2.0,hold=24,cost_per_leg=cost)
        s=stats(p)
        if s: print(f"  cost_per_leg={cost*10000:>4.1f}pip: n={s['n']:<4} PnL=${s['pnl']:>7.2f} Sh={s['sharpe']:>5.2f} MDD=${s['mdd']:>5.2f}")

    print("\n== Walk-forward (4 fold) ==")
    folds=4
    ts_starts=[ts0 + span*i//folds for i in range(folds+1)]
    for i in range(folds):
        p,_=pairs_with_log(eur,gbp,win=60,z_in=2.0,hold=24,ts_filter=(ts_starts[i],ts_starts[i+1]))
        s=stats(p)
        if s: print(f"  fold {i+1}: n={s['n']:<4} PnL=${s['pnl']:>7.2f} Sh={s['sharpe']:>5.2f} MDD=${s['mdd']:>5.2f}")

if __name__=="__main__":
    main()
