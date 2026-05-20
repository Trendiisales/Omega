#!/usr/bin/env python3
"""
Cross-asset pairs probe. Uses log-spread z-score mean reversion.
Tests with 2yr data we already have:
  - XAU vs JPY (safe haven)
  - EUR vs GER40 (Europe macro)
  - JPY vs SPX (risk-on/off)
  - SPX vs NSX (US equity overlap)
"""
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

def pairs_logspread(a_bars, b_bars, win, zin, zout, hold, cost_pct=0.0001):
    """Generic pairs: spread = log(a_close) - log(b_close), fade z-score.
    cost_pct = fractional cost per leg (round-trip = 2 * cost_pct)."""
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
                # PnL in log-return space: r_long_spread = log(ca/ea) - log(cb/eb)
                r = math.log(ca/ea) - math.log(cb/eb)
                if not is_long: r = -r
                cost = 2 * cost_pct
                pnls.append(r - cost)
                active=False
            continue
        if z>zin: is_long=False; es=spreads[i]; ea=common[i][2].c; eb=common[i][1].c; active=True; ei=i
        elif z<-zin: is_long=True; es=spreads[i]; ea=common[i][2].c; eb=common[i][1].c; active=True; ei=i
    return pnls, len(common)

def main():
    bars_h1 = {}
    paths = {
        "EUR": "/Users/jo/Tick/EURUSD_merged.h1.csv",
        "GBP": "/Users/jo/Tick/GBPUSD_merged.h1.csv",
        "JPY": "/Users/jo/Tick/USDJPY_merged.h1.csv",
        "NSX": "/Users/jo/Tick/NSXUSD_merged.h1.csv",
        "SPX": "/Users/jo/Tick/SPXUSD_merged.h1.csv",
        "GER": "/Users/jo/Tick/GER40_merged.h1.csv",
        "XAU": "/Users/jo/Tick/2yr_XAUUSD_tick_fresh.h1.csv",
    }
    for k,p in paths.items():
        if os.path.exists(p):
            bars_h1[k] = load(p)
            print(f"  {k}: h1 bars={len(bars_h1[k])}")

    paths["AUD"] = "/Users/jo/Tick/AUDUSD_merged.h1.csv"
    paths["NZD"] = "/Users/jo/Tick/NZDUSD_merged.h1.csv"
    paths["CAD"] = "/Users/jo/Tick/USDCAD_merged.h1.csv"
    for k in ["AUD","NZD","CAD"]:
        if os.path.exists(paths[k]) and k not in bars_h1:
            bars_h1[k] = load(paths[k])
            print(f"  {k}: h1 bars={len(bars_h1[k])}")
    combos = [
        ("XAU","JPY"), ("EUR","GER"), ("JPY","SPX"), ("SPX","NSX"),
        ("XAU","EUR"), ("XAU","SPX"), ("GER","SPX"), ("GBP","GER"),
        ("EUR","JPY"), ("XAU","NSX"), ("GBP","SPX"),
        ("XAU","AUD"), ("XAU","NZD"), ("XAU","GBP"),
        ("AUD","SPX"), ("AUD","NSX"), ("AUD","GER"),
        ("NZD","SPX"), ("NZD","GBP"), ("AUD","GBP"),
        ("AUD","JPY"), ("NZD","JPY"), ("AUD","EUR"),
        # USDCAD pairs (oil-correlated; no oil tick data so substitute):
        ("CAD","AUD"), ("CAD","NZD"), ("CAD","JPY"),
        ("CAD","EUR"), ("CAD","GBP"), ("CAD","XAU"),
        ("CAD","SPX"), ("CAD","NSX"), ("CAD","GER"),
    ]
    print("\n=== Cross-asset pairs H1 ===")
    print(f"{'pair':<10} {'w':>3} {'zi':>3} {'h':>3}  {'n':>4} {'PnL%':>7} {'WR':>5} {'Sh':>6}")
    print('-'*60)
    for a,b in combos:
        if a not in bars_h1 or b not in bars_h1: continue
        best = None
        for w in [40, 60, 120]:
            for zi in [1.5, 2.0]:
                for ho in [12, 24, 48]:
                    t,nc=pairs_logspread(bars_h1[a], bars_h1[b], w, zi, 0.5, ho, cost_pct=0.0001)
                    s=stats(t)
                    if not s or s["n"]<20: continue
                    if best is None or s["sharpe"]>best[1]["sharpe"]:
                        best = ({"w":w,"zi":zi,"ho":ho}, s)
        if best:
            cfg, s = best
            tag = " ***" if s["sharpe"]>2.0 else (" *" if s["sharpe"]>1.0 else "")
            pnl_pct = s["pnl"] * 100
            print(f"{a:>4}-{b:<5} {cfg['w']:>3} {cfg['zi']:>3} {cfg['ho']:>3}  {s['n']:>4} {pnl_pct:>6.2f}%  {s['wr']:>4.1f}% {s['sharpe']:>5.2f}{tag}")
        else:
            print(f"{a:>4}-{b:<5} no valid")

def deep_dive(a_name, b_name, bars):
    print(f"\n=== Deep grid {a_name}-{b_name} ===")
    print(f"{'w':>3} {'zi':>4} {'zo':>4} {'h':>3}  {'n':>4} {'PnL%':>7} {'WR':>5} {'Sh':>6} {'MDD%':>6}")
    print('-'*60)
    best = None
    for w in [20, 40, 60, 80, 120, 200]:
        for zi in [1.0, 1.5, 2.0, 2.5, 3.0]:
            for zo in [0.0, 0.3, 0.5, 0.8]:
                for ho in [6, 12, 24, 48, 96]:
                    t,_=pairs_logspread(bars[a_name], bars[b_name], w, zi, zo, ho, cost_pct=0.0001)
                    s=stats(t)
                    if not s or s["n"]<20: continue
                    if best is None or s["sharpe"]>best[1]["sharpe"]:
                        best = ((w,zi,zo,ho), s)
    if best:
        (w,zi,zo,ho), s = best
        print(f"BEST: w={w} zi={zi} zo={zo} h={ho}: n={s['n']} PnL={s['pnl']*100:.2f}% WR={s['wr']:.1f}% Sh={s['sharpe']:.2f} MDD={s['mdd']*100:.2f}%")
    return best

if __name__=="__main__":
    main()
    # Deep dive on GBP-GER + JPY-SPX
    bars_h1 = {}
    paths = {
        "EUR": "/Users/jo/Tick/EURUSD_merged.h1.csv",
        "GBP": "/Users/jo/Tick/GBPUSD_merged.h1.csv",
        "JPY": "/Users/jo/Tick/USDJPY_merged.h1.csv",
        "SPX": "/Users/jo/Tick/SPXUSD_merged.h1.csv",
        "GER": "/Users/jo/Tick/GER40_merged.h1.csv",
    }
    for k,p in paths.items():
        if os.path.exists(p): bars_h1[k] = load(p)
    bars_h1["AUD"] = load("/Users/jo/Tick/AUDUSD_merged.h1.csv")
    bars_h1["NZD"] = load("/Users/jo/Tick/NZDUSD_merged.h1.csv")
    bars_h1["CAD"] = load("/Users/jo/Tick/USDCAD_merged.h1.csv")
    bars_h1["NSX"] = load("/Users/jo/Tick/NSXUSD_merged.h1.csv")
    deep_dive("AUD","SPX",bars_h1)
    deep_dive("NZD","SPX",bars_h1)
    deep_dive("AUD","GER",bars_h1)
    deep_dive("AUD","NSX",bars_h1)
    deep_dive("CAD","AUD",bars_h1)
    deep_dive("CAD","NZD",bars_h1)
    deep_dive("CAD","XAU",bars_h1)
    deep_dive("CAD","JPY",bars_h1)
    deep_dive("CAD","EUR",bars_h1)
    deep_dive("CAD","GBP",bars_h1)
    deep_dive("CAD","SPX",bars_h1)
