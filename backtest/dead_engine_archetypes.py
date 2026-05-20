#!/usr/bin/env python3
"""
Test signal archetypes of deleted gold engines against current 2yr XAU data.

Re-runs the conceptual signal of each retired engine (Donchian/Turtle,
StopRunReversal, OverlapFade, WickRejection, ImpulseContinuation,
CompressionBreakout) on daily + H4 XAU to see if any were wrongly killed.

Same rigor: IS/OOS split, both halves positive Sharpe.
"""
import csv, math
from datetime import datetime, timezone
from collections import deque

def load_daily(path):
    bars=[]
    with open(path) as f:
        for line in f:
            p=line.strip().split(",")
            if len(p)<5: continue
            try:
                d=p[0]
                dt=datetime(int(d[:4]), int(d[4:6]), int(d[6:8]))
                bars.append((int(dt.timestamp()), float(p[1]), float(p[2]), float(p[3]), float(p[4])))
            except: continue
    return bars

def load_csv_bars(path):
    bars=[]
    with open(path) as f:
        r=csv.reader(f); next(r,None)
        for row in r:
            try: bars.append((int(row[0]), float(row[1]), float(row[2]), float(row[3]), float(row[4])))
            except: continue
    return bars

def stats(pnls):
    if not pnls: return None
    n=len(pnls); pnl=sum(pnls); wr=100*sum(1 for p in pnls if p>0)/n
    m=pnl/n; v=sum((p-m)**2 for p in pnls)/max(1,n-1); sd=math.sqrt(v)
    sh=(m/sd*math.sqrt(252)) if sd>0 else 0
    return dict(n=n,pnl=pnl,wr=wr,sharpe=sh)

def atr14(bars):
    out=[None]*len(bars); trs=deque(maxlen=14); pc=None
    for i,(_,_,h,l,c) in enumerate(bars):
        tr=h-l
        if pc is not None: tr=max(tr,abs(h-pc),abs(l-pc))
        trs.append(tr); pc=c
        if len(trs)==14: out[i]=sum(trs)/14
    return out

def isoos(bars, sim_fn, *args, ts_split=None, cost=0.0010):
    if not bars: return None, None, None
    mid = ts_split or bars[len(bars)//2][0]
    p_is  = sim_fn(bars, *args, cost=cost, ts_lo=bars[0][0], ts_hi=mid)
    p_oos = sim_fn(bars, *args, cost=cost, ts_lo=mid, ts_hi=bars[-1][0]+1)
    p_ful = sim_fn(bars, *args, cost=cost)
    return stats(p_is), stats(p_oos), stats(p_ful)

# ── ARCHETYPE 1: Turtle / Donchian-N day high break, long-only ──────────────
def turtle(bars, lookback, hold, sl_atr, tp_atr, cost=0.0010, ts_lo=0, ts_hi=10**12):
    pnls=[]; n=len(bars); atrs=atr14(bars)
    active=False; entry=sl=tp=0; is_long=False; ei=0
    for i in range(lookback,n):
        ts=bars[i][0]
        if ts<ts_lo or ts>=ts_hi: continue
        if active:
            h=bars[i][2]; l=bars[i][3]; c=bars[i][4]
            sh_hit=(l<=sl) if is_long else (h>=sl)
            tp_hit=(h>=tp) if is_long else (l<=tp)
            timed=(i-ei)>=hold
            if sh_hit:
                r=(sl-entry)/entry if is_long else (entry-sl)/entry
                pnls.append(r-cost); active=False
            elif tp_hit:
                r=(tp-entry)/entry if is_long else (entry-tp)/entry
                pnls.append(r-cost); active=False
            elif timed:
                r=(c-entry)/entry if is_long else (entry-c)/entry
                pnls.append(r-cost); active=False
            continue
        if atrs[i] is None: continue
        prior_high=max(b[2] for b in bars[i-lookback:i])
        if bars[i][4] > prior_high:
            is_long=True; entry=bars[i][4]
            apct=atrs[i]/entry
            sl=entry*(1-sl_atr*apct); tp=entry*(1+tp_atr*apct)
            active=True; ei=i
    return pnls

# ── ARCHETYPE 2: StopRunReversal — price breaks N-day low, fades back ──────
def stoprun(bars, lookback, hold, sl_atr, tp_atr, cost=0.0010, ts_lo=0, ts_hi=10**12):
    pnls=[]; n=len(bars); atrs=atr14(bars)
    active=False; entry=sl=tp=0; is_long=False; ei=0
    for i in range(lookback,n):
        ts=bars[i][0]
        if ts<ts_lo or ts>=ts_hi: continue
        if active:
            h=bars[i][2]; l=bars[i][3]; c=bars[i][4]
            sh_hit=(l<=sl) if is_long else (h>=sl)
            tp_hit=(h>=tp) if is_long else (l<=tp)
            timed=(i-ei)>=hold
            if sh_hit:
                r=(sl-entry)/entry if is_long else (entry-sl)/entry
                pnls.append(r-cost); active=False
            elif tp_hit:
                r=(tp-entry)/entry if is_long else (entry-tp)/entry
                pnls.append(r-cost); active=False
            elif timed:
                r=(c-entry)/entry if is_long else (entry-c)/entry
                pnls.append(r-cost); active=False
            continue
        if atrs[i] is None: continue
        prior_low=min(b[3] for b in bars[i-lookback:i])
        # Stop-run: low broken intra-bar but close BACK above prior low
        if bars[i][3] < prior_low and bars[i][4] > prior_low:
            is_long=True; entry=bars[i][4]
            apct=atrs[i]/entry
            sl=entry*(1-sl_atr*apct); tp=entry*(1+tp_atr*apct)
            active=True; ei=i
    return pnls

# ── ARCHETYPE 3: WickRejection — close rejects from upper/lower wick ────────
def wickrej(bars, wick_min_pct, hold, sl_atr, tp_atr, cost=0.0010, ts_lo=0, ts_hi=10**12):
    pnls=[]; n=len(bars); atrs=atr14(bars)
    active=False; entry=sl=tp=0; is_long=False; ei=0
    for i in range(14,n):
        ts=bars[i][0]
        if ts<ts_lo or ts>=ts_hi: continue
        if active:
            h=bars[i][2]; l=bars[i][3]; c=bars[i][4]
            sh_hit=(l<=sl) if is_long else (h>=sl)
            tp_hit=(h>=tp) if is_long else (l<=tp)
            timed=(i-ei)>=hold
            if sh_hit:
                r=(sl-entry)/entry if is_long else (entry-sl)/entry
                pnls.append(r-cost); active=False
            elif tp_hit:
                r=(tp-entry)/entry if is_long else (entry-tp)/entry
                pnls.append(r-cost); active=False
            elif timed:
                r=(c-entry)/entry if is_long else (entry-c)/entry
                pnls.append(r-cost); active=False
            continue
        if atrs[i] is None: continue
        o=bars[i][1]; h=bars[i][2]; l=bars[i][3]; c=bars[i][4]
        body=abs(c-o); rng=h-l
        if rng<=0: continue
        upper_wick=(h-max(o,c))/rng
        lower_wick=(min(o,c)-l)/rng
        # Long: long lower wick (rejected lows)
        if lower_wick > wick_min_pct and c > o:
            is_long=True; entry=c
            apct=atrs[i]/entry
            sl=entry*(1-sl_atr*apct); tp=entry*(1+tp_atr*apct)
            active=True; ei=i
        # Short: long upper wick (rejected highs)
        elif upper_wick > wick_min_pct and c < o:
            is_long=False; entry=c
            apct=atrs[i]/entry
            sl=entry*(1+sl_atr*apct); tp=entry*(1-tp_atr*apct)
            active=True; ei=i
    return pnls

# ── ARCHETYPE 4: ImpulseContinuation — large bar then continue direction ────
def impulse(bars, impulse_atr, hold, sl_atr, tp_atr, cost=0.0010, ts_lo=0, ts_hi=10**12):
    pnls=[]; n=len(bars); atrs=atr14(bars)
    active=False; entry=sl=tp=0; is_long=False; ei=0
    for i in range(14,n):
        ts=bars[i][0]
        if ts<ts_lo or ts>=ts_hi: continue
        if active:
            h=bars[i][2]; l=bars[i][3]; c=bars[i][4]
            sh_hit=(l<=sl) if is_long else (h>=sl)
            tp_hit=(h>=tp) if is_long else (l<=tp)
            timed=(i-ei)>=hold
            if sh_hit:
                r=(sl-entry)/entry if is_long else (entry-sl)/entry
                pnls.append(r-cost); active=False
            elif tp_hit:
                r=(tp-entry)/entry if is_long else (entry-tp)/entry
                pnls.append(r-cost); active=False
            elif timed:
                r=(c-entry)/entry if is_long else (entry-c)/entry
                pnls.append(r-cost); active=False
            continue
        if i<15 or atrs[i] is None: continue
        prev=bars[i-1]
        prev_range=prev[2]-prev[3]
        if prev_range > impulse_atr*atrs[i-1]:
            # Continue in prev bar direction
            is_long = prev[4] > prev[1]  # prev close > prev open
            entry=bars[i][1]  # this bar open
            apct=atrs[i]/entry
            sl=entry*(1-sl_atr*apct) if is_long else entry*(1+sl_atr*apct)
            tp=entry*(1+tp_atr*apct) if is_long else entry*(1-tp_atr*apct)
            active=True; ei=i
    return pnls

# ── ARCHETYPE 5: CompressionBreakout — ATR contraction then break ───────────
def compress(bars, contract_ratio, hold, sl_atr, tp_atr, cost=0.0010, ts_lo=0, ts_hi=10**12):
    """ATR(5) < ratio * ATR(20) means contraction. Then close > prev_high = long."""
    pnls=[]; n=len(bars)
    atr5=[None]*n; atr20=[None]*n
    trs=deque(maxlen=20); trs5=deque(maxlen=5); pc=None
    for i,(_,_,h,l,c) in enumerate(bars):
        tr=h-l
        if pc is not None: tr=max(tr,abs(h-pc),abs(l-pc))
        trs.append(tr); trs5.append(tr); pc=c
        if len(trs5)==5: atr5[i]=sum(trs5)/5
        if len(trs)==20: atr20[i]=sum(trs)/20
    active=False; entry=sl=tp=0; is_long=False; ei=0
    for i in range(20,n):
        ts=bars[i][0]
        if ts<ts_lo or ts>=ts_hi: continue
        if active:
            h=bars[i][2]; l=bars[i][3]; c=bars[i][4]
            sh_hit=(l<=sl) if is_long else (h>=sl)
            tp_hit=(h>=tp) if is_long else (l<=tp)
            timed=(i-ei)>=hold
            if sh_hit:
                r=(sl-entry)/entry if is_long else (entry-sl)/entry
                pnls.append(r-cost); active=False
            elif tp_hit:
                r=(tp-entry)/entry if is_long else (entry-tp)/entry
                pnls.append(r-cost); active=False
            elif timed:
                r=(c-entry)/entry if is_long else (entry-c)/entry
                pnls.append(r-cost); active=False
            continue
        if atr5[i] is None or atr20[i] is None: continue
        if atr5[i] >= contract_ratio * atr20[i]: continue
        # Contracted, look for break
        prev=bars[i-1]; cur=bars[i]
        if cur[4] > prev[2]:  # close > prev high
            is_long=True; entry=cur[4]
            apct=atr20[i]/entry
            sl=entry*(1-sl_atr*apct); tp=entry*(1+tp_atr*apct)
            active=True; ei=i
    return pnls

# ── Main test ───────────────────────────────────────────────────────────────
print("Loading XAU data...")
daily = load_daily("/Users/jo/Tick/2yr_XAUUSD_daily.csv")
h4 = load_csv_bars("/Users/jo/Tick/2yr_XAUUSD_tick_fresh.h4.csv")
h1 = load_csv_bars("/Users/jo/Tick/2yr_XAUUSD_tick_fresh.h1.csv")
print(f"Daily: {len(daily)}, H4: {len(h4)}, H1: {len(h1)}")

def report(label, archetype_fn, params_grid, bars, tf_name):
    print(f"\n=== {label} on {tf_name} ===")
    best=None
    for params in params_grid:
        s_is, s_oos, s_ful = isoos(bars, archetype_fn, *params)
        if not s_is or not s_oos or not s_ful: continue
        if s_ful["n"]<20: continue
        if s_is["sharpe"]<=0 or s_oos["sharpe"]<=0: continue
        min_sh = min(s_is["sharpe"], s_oos["sharpe"])
        if best is None or min_sh > best[2]:
            best = (params, s_ful, min_sh, s_is, s_oos)
    if best:
        params, s, mn, s_is, s_oos = best
        print(f"  BEST: params={params}")
        print(f"  IS Sh={s_is['sharpe']:.2f}, OOS Sh={s_oos['sharpe']:.2f}, MIN Sh={mn:.2f}")
        print(f"  FUL: n={s['n']} PnL={s['pnl']*100:.2f}% WR={s['wr']:.1f}% Sh={s['sharpe']:.2f}")
    else:
        print(f"  NO ROBUST CONFIG (both halves positive)")

# Turtle / Donchian (S50 X1 retired TurtleTick)
report("TURTLE/Donchian (S50 X1)", turtle, [
    (lb, h, sl, tp) for lb in [10,20,40] for h in [5,10,20] for sl in [1.5,2.0] for tp in [3.0,5.0,8.0]
], daily, "DAILY")

# StopRunReversal (S50 X2)
report("STOPRUN_REV (S50 X2)", stoprun, [
    (lb, h, sl, tp) for lb in [5,10,20] for h in [5,10,20] for sl in [1.0,1.5,2.0] for tp in [2.0,3.0,5.0]
], daily, "DAILY")

report("STOPRUN_REV (S50 X2)", stoprun, [
    (lb, h, sl, tp) for lb in [10,20,30,50] for h in [6,12,24] for sl in [1.0,1.5,2.0] for tp in [2.0,3.0,5.0]
], h4, "H4")

# WickRejection (shelved)
report("WICK_REJECTION (shelved)", wickrej, [
    (wm, h, sl, tp) for wm in [0.4,0.5,0.6,0.7] for h in [3,5,10] for sl in [1.0,1.5,2.0] for tp in [2.0,3.0,5.0]
], daily, "DAILY")

report("WICK_REJECTION (shelved)", wickrej, [
    (wm, h, sl, tp) for wm in [0.4,0.5,0.6,0.7] for h in [6,12,24] for sl in [1.0,1.5,2.0] for tp in [2.0,3.0,5.0]
], h4, "H4")

# ImpulseContinuation (retired)
report("IMPULSE_CONT (retired)", impulse, [
    (im, h, sl, tp) for im in [1.5,2.0,2.5] for h in [3,5,10] for sl in [1.0,1.5,2.0] for tp in [2.0,3.0,5.0]
], daily, "DAILY")

# CompressionBreakout (S16 retired)
report("COMPRESS_BREAK (S16)", compress, [
    (cr, h, sl, tp) for cr in [0.6,0.7,0.8] for h in [5,10,20] for sl in [1.5,2.0] for tp in [3.0,5.0,8.0]
], daily, "DAILY")
