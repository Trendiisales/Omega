#!/usr/bin/env python3
"""
Test #3: EUR-GBP pairs + regime filter.

Hypothesis: spread mean-reversion is reliable in range markets, unreliable in
strong trending markets. Filter trades by both legs' EMA alignment:
  - 'aligned': EUR & GBP both trending same direction -> SKIP (trend regime)
  - 'diverging': EUR up GBP down (or vice versa) -> TRADE (range / mean-rev regime)
  - 'flat': both in chop -> TRADE

Optional inverse: 'aligned-only' lets us test if trending regimes actually fade well.
"""
import os, csv, math
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

def ema_series(closes, period):
    out=[None]*len(closes); a=2/(period+1); v=None
    for i,c in enumerate(closes):
        v = c if v is None else a*c+(1-a)*v
        if i>=period: out[i]=v
    return out

def pairs_with_filter(eur, gbp, win, zin, zout, hold, regime_mode="all",
                      ema_fast=20, ema_slow=50, cost_pct=0.0001):
    """
    regime_mode:
      all       - no filter (baseline)
      diverging - only enter when legs trend opposite directions
      aligned   - only enter when legs trend same direction
      flat      - only enter when both legs flat (|EMA20-EMA50|/EMA50 < 0.1%)
    """
    eur_idx={b.ts:b for b in eur}
    common=[(b.ts, b, eur_idx[b.ts]) for b in gbp if b.ts in eur_idx]
    if len(common)<200: return []
    eur_closes=[c[2].c for c in common]
    gbp_closes=[c[1].c for c in common]
    e_fast = ema_series(eur_closes, ema_fast)
    e_slow = ema_series(eur_closes, ema_slow)
    g_fast = ema_series(gbp_closes, ema_fast)
    g_slow = ema_series(gbp_closes, ema_slow)
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
                r=math.log(ca/ea) - math.log(cb/eb)
                if not is_long: r=-r
                pnls.append(r - 2*cost_pct)
                active=False
            continue
        # Regime filter
        if regime_mode != "all":
            if e_fast[i] is None or e_slow[i] is None or g_fast[i] is None or g_slow[i] is None:
                continue
            eur_trend = "up" if e_fast[i] > e_slow[i] else "down"
            gbp_trend = "up" if g_fast[i] > g_slow[i] else "down"
            eur_sep = abs(e_fast[i] - e_slow[i]) / e_slow[i]
            gbp_sep = abs(g_fast[i] - g_slow[i]) / g_slow[i]
            eur_flat = eur_sep < 0.001
            gbp_flat = gbp_sep < 0.001
            if regime_mode == "diverging":
                if eur_trend == gbp_trend and not (eur_flat or gbp_flat): continue
            elif regime_mode == "aligned":
                if eur_trend != gbp_trend: continue
                if eur_flat or gbp_flat: continue
            elif regime_mode == "flat":
                if not (eur_flat and gbp_flat): continue
        if z>zin: is_long=False; ea=common[i][2].c; eb=common[i][1].c; active=True; ei=i
        elif z<-zin: is_long=True; ea=common[i][2].c; eb=common[i][1].c; active=True; ei=i
    return pnls

def main():
    eur = load("/Users/jo/Tick/EURUSD_merged.h1.csv")
    gbp = load("/Users/jo/Tick/GBPUSD_merged.h1.csv")
    print(f"EUR h1={len(eur)} GBP h1={len(gbp)}")

    base_cfg = dict(win=120, zin=1.5, zout=0.5, hold=48, cost_pct=0.00015)
    print(f"\nBase config: {base_cfg}")
    print(f"\n{'regime':<12} {'n':>4} {'PnL%':>8} {'WR':>5} {'Sh':>6} {'MDD%':>5}")
    print('-'*50)
    for mode in ["all", "diverging", "aligned", "flat"]:
        p = pairs_with_filter(eur, gbp, regime_mode=mode, **base_cfg)
        s = stats(p)
        if not s or s["n"]<5: print(f"  {mode:<12} insufficient trades"); continue
        print(f"  {mode:<10} {s['n']:>4} {s['pnl']*100:>7.2f}% {s['wr']:>4.1f}% {s['sharpe']:>5.2f} {s['mdd']*100:>4.2f}%")

    # Try with EMA periods optimization
    print(f"\n=== Grid: regime=diverging, vary EMA fast/slow ===")
    best = None
    for ef in [10, 20, 30, 50]:
        for es in [40, 60, 100, 150, 200]:
            if es <= ef: continue
            p = pairs_with_filter(eur, gbp, ema_fast=ef, ema_slow=es, regime_mode="diverging", **base_cfg)
            s = stats(p)
            if not s or s["n"]<20: continue
            print(f"  ef={ef:<3} es={es:<3}: n={s['n']:<4} PnL={s['pnl']*100:>6.2f}% WR={s['wr']:>4.1f}% Sh={s['sharpe']:>5.2f}")
            if best is None or s["sharpe"]>best[1]["sharpe"]:
                best = ((ef,es), s)

    if best:
        (ef,es), s = best
        print(f"\nBEST diverging filter: ef={ef} es={es}: Sh={s['sharpe']:.2f} n={s['n']} PnL={s['pnl']*100:.2f}%")

    # H4 (4hr) regime filter -- looks at HTF trend
    print(f"\n=== H4 regime filter (use HTF EMA on H4 prices) ===")
    eur_h4 = load("/Users/jo/Tick/EURUSD_merged.h4.csv")
    gbp_h4 = load("/Users/jo/Tick/GBPUSD_merged.h4.csv")
    # Map each H1 ts to its H4 bucket. Use EMA on H4 to gate H1 entries.
    eur_h4_by_bucket = {b.ts // 14400 * 14400: b for b in eur_h4}
    gbp_h4_by_bucket = {b.ts // 14400 * 14400: b for b in gbp_h4}
    # For each H1 bar, find latest H4 close <= H1 ts. Build EMA stream from H4 close series.
    eur_h4_sorted = sorted(eur_h4, key=lambda x:x.ts)
    gbp_h4_sorted = sorted(gbp_h4, key=lambda x:x.ts)
    eur_h4_closes = [b.c for b in eur_h4_sorted]
    gbp_h4_closes = [b.c for b in gbp_h4_sorted]
    eur_h4_ts     = [b.ts for b in eur_h4_sorted]
    gbp_h4_ts     = [b.ts for b in gbp_h4_sorted]
    eur_h4_ef = ema_series(eur_h4_closes, 10)
    eur_h4_es = ema_series(eur_h4_closes, 30)
    gbp_h4_ef = ema_series(gbp_h4_closes, 10)
    gbp_h4_es = ema_series(gbp_h4_closes, 30)

    # Custom run that uses H4 EMA aligned by ts
    import bisect
    eur_idx = {b.ts: b for b in eur}
    common = [(b.ts, b, eur_idx[b.ts]) for b in gbp if b.ts in eur_idx]
    spreads = [math.log(c[2].c)-math.log(c[1].c) for c in common]
    pnls = []; active = False; ea_p = 0; eb_p = 0; is_long = False; ei = 0
    for i in range(len(spreads)):
        if i < 120: continue
        ts_now = common[i][0]
        wdat = spreads[i-120:i]
        m = sum(wdat)/120; sd = math.sqrt(sum((x-m)**2 for x in wdat)/119)
        if sd <= 0: continue
        z = (spreads[i]-m)/sd
        if active:
            cz = (spreads[i]-m)/sd
            hit = (cz<=0.5) if is_long else (cz>=-0.5)
            timed = (i-ei)>=48
            if hit or timed:
                ca=common[i][2].c; cb=common[i][1].c
                r=math.log(ca/ea_p)-math.log(cb/eb_p)
                if not is_long: r=-r
                pnls.append(r - 2*0.00015)
                active = False
            continue
        # H4 EMA filter: find most recent H4 bar with index_h4 such that ts_h4 < ts_now
        idx_e = bisect.bisect_right(eur_h4_ts, ts_now) - 1
        idx_g = bisect.bisect_right(gbp_h4_ts, ts_now) - 1
        if idx_e < 30 or idx_g < 30: continue
        if eur_h4_ef[idx_e] is None or eur_h4_es[idx_e] is None: continue
        if gbp_h4_ef[idx_g] is None or gbp_h4_es[idx_g] is None: continue
        eur_h4_trend = "up" if eur_h4_ef[idx_e] > eur_h4_es[idx_e] else "down"
        gbp_h4_trend = "up" if gbp_h4_ef[idx_g] > gbp_h4_es[idx_g] else "down"
        # SKIP if both aligned (trending)
        if eur_h4_trend == gbp_h4_trend: continue
        if abs(z) < 1.5: continue
        if z > 1.5: is_long=False; ea_p=common[i][2].c; eb_p=common[i][1].c; active=True; ei=i
        elif z < -1.5: is_long=True; ea_p=common[i][2].c; eb_p=common[i][1].c; active=True; ei=i

    s = stats(pnls)
    if s: print(f"  H4 diverging filter: n={s['n']:<3} PnL={s['pnl']*100:>6.2f}% WR={s['wr']:>4.1f}% Sh={s['sharpe']:>5.2f}")

def isoos_wf(eur, gbp, cfg, label):
    eur_idx={b.ts for b in eur}
    common_ts=sorted(b.ts for b in gbp if b.ts in eur_idx)
    if not common_ts: return
    ts_lo, ts_hi = common_ts[0], common_ts[-1]
    mid = ts_lo + (ts_hi-ts_lo)//2

    def run_within(lo, hi):
        eur_sub = [b for b in eur if lo<=b.ts<hi]
        gbp_sub = [b for b in gbp if lo<=b.ts<hi]
        return pairs_with_filter(eur_sub, gbp_sub, **cfg)

    print(f"\n=== IS/OOS + WF: {label} ===")
    is_p = run_within(ts_lo, mid)
    oos_p = run_within(mid, ts_hi+1)
    full_p = run_within(ts_lo, ts_hi+1)
    for tag, p in [("IS ",is_p),("OOS",oos_p),("FUL",full_p)]:
        s = stats(p)
        if s: print(f"  {tag} n={s['n']:<3} PnL={s['pnl']*100:>6.2f}% WR={s['wr']:>4.1f}% Sh={s['sharpe']:>5.2f}")
    pos = 0
    for f in range(4):
        lo = ts_lo + (ts_hi-ts_lo)*f//4
        hi = ts_lo + (ts_hi-ts_lo)*(f+1)//4
        p = run_within(lo, hi)
        s = stats(p)
        if s:
            print(f"  F{f+1} n={s['n']:<3} PnL={s['pnl']*100:>6.2f}% Sh={s['sharpe']:>5.2f}")
            if s["sharpe"]>0: pos+=1
    print(f"  Positive folds: {pos}/4")

if __name__=="__main__":
    main()
    eur = load("/Users/jo/Tick/EURUSD_merged.h1.csv")
    gbp = load("/Users/jo/Tick/GBPUSD_merged.h1.csv")
    base = dict(win=120, zin=1.5, zout=0.5, hold=48, cost_pct=0.00015)
    isoos_wf(eur, gbp, dict(base, regime_mode="all"),       "baseline")
    isoos_wf(eur, gbp, dict(base, regime_mode="diverging"), "diverging")
