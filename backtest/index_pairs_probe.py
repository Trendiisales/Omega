#!/usr/bin/env python3
"""
Index pairs spread mean-reversion probe.
Combos: SP-NQ, SP-GER, NQ-GER, NQ-NSX (NSX is NAS100 too -- maybe near-clone).
Spread = sym_a_close - hedge*sym_b_close (hedge by rolling ratio).
"""
import os, csv, math
from collections import deque
from dataclasses import dataclass

# H1 bars for each. usd_per_pt at 0.01 lot.
SYMBOLS = {
    "SPXUSD":  dict(usd_per_pt=50.0, name="SP500"),   # $0.50/pt at 0.01 lot (S&P500 CFD)
    "NSXUSD":  dict(usd_per_pt=1.0,  name="NAS100"),
    "GER40":   dict(usd_per_pt=25.0, name="DAX"),
}

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

def pairs(h1_a, h1_b, sym_a, sym_b, win=120, z_in=2.0, z_out=0.5, hold=24,
          cost_pt_a=0.5, cost_pt_b=2.0, use_log_ratio=True):
    """Spread = log(a) - log(b) if use_log_ratio else a/b_ratio adjusted."""
    a_idx={b.ts:b for b in h1_a}
    common=[(b.ts, b, a_idx[b.ts]) for b in h1_b if b.ts in a_idx]
    if len(common)<200: return [], 0
    if use_log_ratio:
        spreads=[math.log(c[2].c) - math.log(c[1].c) for c in common]
    else:
        # raw price spread (only meaningful if same scale)
        spreads=[c[2].c - c[1].c for c in common]
    ts_list=[c[0] for c in common]
    pnls=[]
    active=False; entry_spread=0; entry_a=0; entry_b=0; is_long=False; entry_idx=0
    cfg_a=SYMBOLS[sym_a]; cfg_b=SYMBOLS[sym_b]
    for i in range(len(spreads)):
        if i<win: continue
        wdat=spreads[i-win:i]
        m=sum(wdat)/win
        sd=math.sqrt(sum((x-m)**2 for x in wdat)/(win-1))
        if sd<=0: continue
        z=(spreads[i]-m)/sd
        if active:
            cz=(spreads[i]-m)/sd
            hit=(cz<=z_out) if is_long else (cz>=-z_out)
            stop=(cz<-3.5) if is_long else (cz>3.5)
            timed=(i-entry_idx)>=hold
            if hit or stop or timed:
                cur_a=common[i][2].c; cur_b=common[i][1].c
                # PnL: long-spread = long a, short b (each 0.01 lot)
                # use log diffs -> need conversion via dPrice ~ a * dlog(a)
                # Simpler: P&L based on actual close prices
                a_pnl = (1 if is_long else -1) * (cur_a - entry_a) * cfg_a["usd_per_pt"] * 0.01
                b_pnl = (-1 if is_long else 1) * (cur_b - entry_b) * cfg_b["usd_per_pt"] * 0.01
                cost = cost_pt_a * cfg_a["usd_per_pt"] * 0.01 + cost_pt_b * cfg_b["usd_per_pt"] * 0.01
                pnls.append(a_pnl + b_pnl - cost)
                active=False
            continue
        if z>z_in: is_long=False; entry_spread=spreads[i]; entry_a=common[i][2].c; entry_b=common[i][1].c; active=True; entry_idx=i
        elif z<-z_in: is_long=True; entry_spread=spreads[i]; entry_a=common[i][2].c; entry_b=common[i][1].c; active=True; entry_idx=i
    return pnls, len(common)

def main():
    paths = {
        "SPXUSD": "/Users/jo/Tick/SPXUSD_merged.h1.csv",
        "NSXUSD": "/Users/jo/Tick/NSXUSD_merged.h1.csv",
        "GER40":  "/Users/jo/Tick/GER40_merged.h1.csv",
    }
    loaded = {}
    for s,p in paths.items():
        if os.path.exists(p):
            loaded[s] = load(p)
            print(f"  {s}: h1 bars={len(loaded[s])}")
    combos = [("SPXUSD","NSXUSD"), ("SPXUSD","GER40"), ("NSXUSD","GER40")]
    print("\n=== INDEX PAIRS SWEEP (log-spread, cost=spread_pts) ===")
    for sa,sb in combos:
        if sa not in loaded or sb not in loaded: continue
        print(f"\n-- {sa} vs {sb} --")
        for w in [40, 60, 120, 240]:
            for zi in [1.5, 2.0, 2.5]:
                for ho in [12, 24, 48]:
                    t,nc=pairs(loaded[sa], loaded[sb], sa, sb,
                               win=w, z_in=zi, hold=ho)
                    s=stats(t)
                    if not s or s["n"]<20: continue
                    if s["sharpe"]<0.5: continue
                    tag=" ***" if s["sharpe"]>2.0 else (" *" if s["sharpe"]>1.0 else "")
                    print(f"  w={w:<3} zi={zi} h={ho:<2}: n={s['n']:<3} PnL=${s['pnl']:>9.2f} WR={s['wr']:>4.1f}% Sh={s['sharpe']:>5.2f} MDD=${s['mdd']:>7.2f}{tag}")

if __name__=="__main__":
    main()
