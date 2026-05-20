#!/usr/bin/env python3
"""
Test #4: EUR/GBP synthetic cross mean-reversion (SINGLE instrument trade).

If EUR-GBP pairs trade has real edge via spread mean-reversion, the SAME edge
should be visible as mean-reversion on the synthetic EURGBP cross
(price_t = EURUSD_t / GBPUSD_t).

If it works, broker can trade the EURGBP cross directly = ONE leg = no
desync risk, half the spread cost, simpler engine.
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

def build_synthetic_cross(eur, gbp):
    """Build synthetic EURGBP = EUR/GBP from aligned H1 bars."""
    eur_idx = {b.ts: b for b in eur}
    out = []
    for b in gbp:
        if b.ts in eur_idx:
            e = eur_idx[b.ts]
            # synthetic close
            c = e.c / b.c
            o = e.o / b.o
            h = max(e.h / b.l, e.l / b.h, c, o)  # approximate
            l = min(e.l / b.h, e.h / b.l, c, o)
            out.append(Bar(b.ts, o, h, l, c))
    return out

def run_zscore_meanrev(bars, win, zin, zout, hold, cost_pct=0.0001, ts_lo=0, ts_hi=10**12):
    """Standard z-score fade on single time series."""
    pnls = []
    active = False; entry = 0; is_long = False; entry_idx = 0
    closes = [b.c for b in bars]
    for i, b in enumerate(bars):
        if not (ts_lo <= b.ts < ts_hi): continue
        if i < win: continue
        win_data = closes[i-win:i]
        m = sum(win_data) / win
        v = sum((x-m)**2 for x in win_data) / (win-1)
        sd = math.sqrt(v)
        if sd <= 0: continue
        z = (b.c - m) / sd
        if active:
            cz = (b.c - m) / sd
            hit = (cz <= zout) if is_long else (cz >= -zout)
            stop = (cz < -3.5) if is_long else (cz > 3.5)
            timed = (i - entry_idx) >= hold
            if hit or stop or timed:
                r = math.log(b.c / entry)
                if not is_long: r = -r
                pnls.append(r - cost_pct)  # single leg cost
                active = False
            continue
        if z > zin:
            is_long = False; entry = b.c; active = True; entry_idx = i
        elif z < -zin:
            is_long = True; entry = b.c; active = True; entry_idx = i
    return pnls

def main():
    eur = load("/Users/jo/Tick/EURUSD_merged.h1.csv")
    gbp = load("/Users/jo/Tick/GBPUSD_merged.h1.csv")
    print(f"EUR h1={len(eur)} GBP h1={len(gbp)}")
    synth = build_synthetic_cross(eur, gbp)
    print(f"Synthetic EURGBP bars: {len(synth)}")

    print("\n=== Sweep on synthetic EURGBP (single instrument fade) ===")
    print(f"{'w':>3} {'zi':>4} {'zo':>4} {'h':>3}  {'n':>4} {'PnL%':>7} {'WR':>5} {'Sh':>6} {'MDD%':>6}")
    print('-'*60)
    best = None
    for w in [40, 60, 80, 120, 200]:
        for zi in [1.0, 1.5, 2.0, 2.5, 3.0]:
            for zo in [0.0, 0.3, 0.5]:
                for ho in [6, 12, 24, 48, 72]:
                    p = run_zscore_meanrev(synth, w, zi, zo, ho, cost_pct=0.0001)
                    s = stats(p)
                    if not s or s["n"]<20: continue
                    if s["sharpe"]>3.0:
                        print(f"{w:>3} {zi:>4} {zo:>4} {ho:>3}  {s['n']:>4} {s['pnl']*100:>6.2f}% {s['wr']:>4.1f}% {s['sharpe']:>5.2f} {s['mdd']*100:>5.2f}%")
                    if best is None or s["sharpe"]>best[1]["sharpe"]:
                        best = ((w,zi,zo,ho), s)
    if best:
        (w,zi,zo,h), s = best
        print(f"\nBEST: w={w} zi={zi} zo={zo} h={h}: n={s['n']} PnL={s['pnl']*100:.2f}% Sh={s['sharpe']:.2f}")

    # IS/OOS top config
    if best:
        (w,zi,zo,h), s = best
        cfg = dict(win=w, zin=zi, zout=zo, hold=h, cost_pct=0.0001)
        ts_lo = synth[0].ts; ts_hi = synth[-1].ts
        mid = ts_lo + (ts_hi-ts_lo)//2
        print(f"\n=== IS/OOS on best config ===")
        is_p = run_zscore_meanrev(synth, **cfg, ts_lo=ts_lo, ts_hi=mid)
        oos_p = run_zscore_meanrev(synth, **cfg, ts_lo=mid, ts_hi=ts_hi+1)
        for tag, p in [("IS ",is_p),("OOS",oos_p)]:
            s2 = stats(p)
            if s2: print(f"  {tag} n={s2['n']:<3} PnL={s2['pnl']*100:>6.2f}% WR={s2['wr']:>4.1f}% Sh={s2['sharpe']:>5.2f}")

        # WF 4-fold
        print(f"\n=== Walk-forward 4-fold ===")
        pos = 0
        for f in range(4):
            lo = ts_lo + (ts_hi-ts_lo)*f//4
            hi = ts_lo + (ts_hi-ts_lo)*(f+1)//4
            p = run_zscore_meanrev(synth, **cfg, ts_lo=lo, ts_hi=hi)
            s2 = stats(p)
            if s2:
                print(f"  F{f+1} n={s2['n']:<3} PnL={s2['pnl']*100:>6.2f}% Sh={s2['sharpe']:>5.2f}")
                if s2["sharpe"]>0: pos+=1
        print(f"  Positive: {pos}/4")

        # Cost stress
        print(f"\n=== Cost stress ===")
        for c in [0.00005, 0.00010, 0.00015, 0.00020, 0.00030]:
            cfg2 = dict(cfg); cfg2["cost_pct"]=c
            p = run_zscore_meanrev(synth, **cfg2)
            s2 = stats(p)
            if s2: print(f"  cost={c*10000:.1f}bps: n={s2['n']:<3} PnL={s2['pnl']*100:>6.2f}% Sh={s2['sharpe']:>5.2f}")

if __name__=="__main__":
    main()
