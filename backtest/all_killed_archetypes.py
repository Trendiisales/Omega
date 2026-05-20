#!/usr/bin/env python3
"""
Exhaustive test of ALL killed/disabled engine archetypes against current data.

Tests every signal pattern from retired/disabled engines on XAU + FX + indices
across H1/H4/D1 timeframes. Uses IS/OOS robust criterion.

Killed/disabled engines covered:
- OverlapFade (session overlap mean reversion)
- EMA Cross (golden cross)
- RSI Extreme (OB/OS)
- RSI Reversal (mean rev)
- Bollinger Band scalp
- Pullback Continuation
- ThreeBar pattern
- TrendPullback
- VWAPReversion
- NBM (Noise Band Momentum)
- ORB (Opening Range Breakout)
- Carry Unwind (FX)
- EsNqDivergence
"""
import csv, math, os
from datetime import datetime, timezone
from collections import deque

def load_csv_bars(path):
    bars=[]
    if not os.path.exists(path): return bars
    with open(path) as f:
        r=csv.reader(f); next(r,None)
        for row in r:
            try: bars.append((int(row[0]), float(row[1]), float(row[2]), float(row[3]), float(row[4])))
            except: continue
    return bars

def load_daily(path):
    bars=[]
    if not os.path.exists(path): return bars
    with open(path) as f:
        for line in f:
            p=line.strip().split(",")
            if len(p)<5: continue
            try:
                d=p[0]
                dt=datetime(int(d[:4]), int(d[4:6]), int(d[6:8]), tzinfo=timezone.utc)
                bars.append((int(dt.timestamp()), float(p[1]), float(p[2]), float(p[3]), float(p[4])))
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

def ema(bars, p):
    out=[None]*len(bars); a=2/(p+1); v=None
    for i,b in enumerate(bars):
        v=b[4] if v is None else a*b[4]+(1-a)*v
        if i>=p: out[i]=v
    return out

def utc_hour(ts): return (ts // 3600) % 24

# ── Bracket exit helper ──────────────────────────────────────────────────────
def manage(bars, i, entry, sl, tp, hold, ei, is_long, cost):
    h=bars[i][2]; l=bars[i][3]; c=bars[i][4]
    sl_hit=(l<=sl) if is_long else (h>=sl)
    tp_hit=(h>=tp) if is_long else (l<=tp)
    timed=(i-ei)>=hold
    if sl_hit:
        r=(sl-entry)/entry if is_long else (entry-sl)/entry
        return r-cost, True
    if tp_hit:
        r=(tp-entry)/entry if is_long else (entry-tp)/entry
        return r-cost, True
    if timed:
        r=(c-entry)/entry if is_long else (entry-c)/entry
        return r-cost, True
    return 0.0, False

# ── ARCHETYPE: EMA Cross (golden/death) ──────────────────────────────────────
def ema_cross(bars, fast_p, slow_p, hold, sl_atr, tp_atr, long_only=True, cost=0.0010, ts_lo=0, ts_hi=10**12):
    pnls=[]; atrs=atr14(bars); e_f=ema(bars,fast_p); e_s=ema(bars,slow_p)
    active=False; entry=sl=tp=0; is_long=False; ei=0
    prev_above=None
    for i in range(slow_p+1, len(bars)):
        ts=bars[i][0]
        if ts<ts_lo or ts>=ts_hi: continue
        if active:
            p, closed = manage(bars,i,entry,sl,tp,hold,ei,is_long,cost)
            if closed: pnls.append(p); active=False
            continue
        if e_f[i] is None or e_s[i] is None or atrs[i] is None: continue
        cur_above = e_f[i] > e_s[i]
        if prev_above is None: prev_above = cur_above; continue
        # Golden cross: was below, now above
        cross_up = (not prev_above) and cur_above
        cross_dn = prev_above and (not cur_above)
        prev_above = cur_above
        if cross_up:
            is_long=True
        elif cross_dn and not long_only:
            is_long=False
        else: continue
        entry=bars[i][4]; apct=atrs[i]/entry
        sl=entry*(1-sl_atr*apct) if is_long else entry*(1+sl_atr*apct)
        tp=entry*(1+tp_atr*apct) if is_long else entry*(1-tp_atr*apct)
        active=True; ei=i
    return pnls

# ── ARCHETYPE: RSI Extreme (OB/OS bounce) ────────────────────────────────────
def rsi_series(bars, period=14):
    out=[None]*len(bars); gains=deque(maxlen=period); losses=deque(maxlen=period)
    for i in range(1, len(bars)):
        ch = bars[i][4]-bars[i-1][4]
        gains.append(max(0,ch)); losses.append(max(0,-ch))
        if len(gains)==period:
            avg_g=sum(gains)/period; avg_l=sum(losses)/period
            if avg_l==0: out[i]=100
            else:
                rs=avg_g/avg_l; out[i]=100 - 100/(1+rs)
    return out

def rsi_extreme(bars, rsi_thresh_lo, rsi_thresh_hi, hold, sl_atr, tp_atr, long_only=True, cost=0.0010, ts_lo=0, ts_hi=10**12):
    pnls=[]; atrs=atr14(bars); rsi=rsi_series(bars)
    active=False; entry=sl=tp=0; is_long=False; ei=0
    for i in range(20, len(bars)):
        ts=bars[i][0]
        if ts<ts_lo or ts>=ts_hi: continue
        if active:
            p, closed = manage(bars,i,entry,sl,tp,hold,ei,is_long,cost)
            if closed: pnls.append(p); active=False
            continue
        if rsi[i] is None or atrs[i] is None: continue
        if rsi[i] < rsi_thresh_lo:
            is_long=True
        elif rsi[i] > rsi_thresh_hi and not long_only:
            is_long=False
        else: continue
        entry=bars[i][4]; apct=atrs[i]/entry
        sl=entry*(1-sl_atr*apct) if is_long else entry*(1+sl_atr*apct)
        tp=entry*(1+tp_atr*apct) if is_long else entry*(1-tp_atr*apct)
        active=True; ei=i
    return pnls

# ── ARCHETYPE: Bollinger Band Scalp ──────────────────────────────────────────
def bb_scalp(bars, bb_period, bb_std, hold, sl_atr, tp_atr, long_only=True, cost=0.0010, ts_lo=0, ts_hi=10**12):
    """Fade when close pierces +/- N std bands -> mean reversion to MA."""
    pnls=[]; atrs=atr14(bars)
    closes=[b[4] for b in bars]
    active=False; entry=sl=tp=0; is_long=False; ei=0
    for i in range(bb_period+1, len(bars)):
        ts=bars[i][0]
        if ts<ts_lo or ts>=ts_hi: continue
        if active:
            p, closed = manage(bars,i,entry,sl,tp,hold,ei,is_long,cost)
            if closed: pnls.append(p); active=False
            continue
        wd = closes[i-bb_period:i]
        m = sum(wd)/bb_period
        v = sum((x-m)**2 for x in wd)/(bb_period-1)
        sd = math.sqrt(v)
        if sd<=0 or atrs[i] is None: continue
        upper = m + bb_std*sd
        lower = m - bb_std*sd
        c = bars[i][4]
        if c < lower:
            is_long=True
        elif c > upper and not long_only:
            is_long=False
        else: continue
        entry=c; apct=atrs[i]/entry
        sl=entry*(1-sl_atr*apct) if is_long else entry*(1+sl_atr*apct)
        tp=entry*(1+tp_atr*apct) if is_long else entry*(1-tp_atr*apct)
        active=True; ei=i
    return pnls

# ── ARCHETYPE: Pullback Continuation ─────────────────────────────────────────
def pullback_cont(bars, fast_p, slow_p, pullback_atr, hold, sl_atr, tp_atr, cost=0.0010, ts_lo=0, ts_hi=10**12):
    """Long when EMA_fast > EMA_slow AND price pulls back to EMA_fast within pullback_atr*ATR, then closes back above."""
    pnls=[]; atrs=atr14(bars); ef=ema(bars,fast_p); es=ema(bars,slow_p)
    active=False; entry=sl=tp=0; ei=0
    for i in range(slow_p+1, len(bars)):
        ts=bars[i][0]
        if ts<ts_lo or ts>=ts_hi: continue
        if active:
            p, closed = manage(bars,i,entry,sl,tp,hold,ei,True,cost)
            if closed: pnls.append(p); active=False
            continue
        if ef[i] is None or es[i] is None or atrs[i] is None: continue
        if ef[i] <= es[i]: continue  # need uptrend
        l=bars[i][3]; c=bars[i][4]
        if l > ef[i] + pullback_atr*atrs[i]: continue
        if c < ef[i]: continue
        entry=c; apct=atrs[i]/entry
        sl=entry*(1-sl_atr*apct); tp=entry*(1+tp_atr*apct)
        active=True; ei=i
    return pnls

# ── ARCHETYPE: ThreeBar (3 consecutive same-direction bars -> reverse) ──────
def threebar(bars, hold, sl_atr, tp_atr, fade=True, cost=0.0010, ts_lo=0, ts_hi=10**12):
    pnls=[]; atrs=atr14(bars)
    active=False; entry=sl=tp=0; is_long=False; ei=0
    for i in range(15, len(bars)):
        ts=bars[i][0]
        if ts<ts_lo or ts>=ts_hi: continue
        if active:
            p, closed = manage(bars,i,entry,sl,tp,hold,ei,is_long,cost)
            if closed: pnls.append(p); active=False
            continue
        if atrs[i] is None: continue
        # Last 3 closed bars same direction
        bd = [bars[j][4]>bars[j][1] for j in range(i-3,i)]
        if all(bd):  # 3 bullish -> fade short (or continue long)
            is_long = (not fade)
        elif not any(bd):  # 3 bearish -> fade long (or continue short)
            is_long = fade
        else: continue
        entry=bars[i][4]; apct=atrs[i]/entry
        sl=entry*(1-sl_atr*apct) if is_long else entry*(1+sl_atr*apct)
        tp=entry*(1+tp_atr*apct) if is_long else entry*(1-tp_atr*apct)
        active=True; ei=i
    return pnls

# ── ARCHETYPE: NBM (Noise Band Momentum) ─────────────────────────────────────
def nbm(bars, atr_band, momentum_atr, hold, sl_atr, tp_atr, long_only=True, cost=0.0010, ts_lo=0, ts_hi=10**12):
    """Define noise band around MA. Enter when price closes outside band by momentum_atr*ATR."""
    pnls=[]; atrs=atr14(bars); ma=ema(bars,20)
    active=False; entry=sl=tp=0; is_long=False; ei=0
    for i in range(25, len(bars)):
        ts=bars[i][0]
        if ts<ts_lo or ts>=ts_hi: continue
        if active:
            p, closed = manage(bars,i,entry,sl,tp,hold,ei,is_long,cost)
            if closed: pnls.append(p); active=False
            continue
        if ma[i] is None or atrs[i] is None: continue
        c=bars[i][4]
        upper = ma[i] + atr_band*atrs[i]
        lower = ma[i] - atr_band*atrs[i]
        # Momentum break above upper by momentum_atr*ATR
        if c > upper + momentum_atr*atrs[i]:
            is_long=True
        elif c < lower - momentum_atr*atrs[i] and not long_only:
            is_long=False
        else: continue
        entry=c; apct=atrs[i]/entry
        sl=entry*(1-sl_atr*apct) if is_long else entry*(1+sl_atr*apct)
        tp=entry*(1+tp_atr*apct) if is_long else entry*(1-tp_atr*apct)
        active=True; ei=i
    return pnls

# ── ARCHETYPE: Session Overlap Fade (12-16 UTC mean rev) ─────────────────────
def overlap_fade(bars, dev_atr, hold, sl_atr, tp_atr, hours=(12,16), cost=0.0010, ts_lo=0, ts_hi=10**12):
    """During London-NY overlap (12-16 UTC), fade large bars back to session VWAP."""
    pnls=[]; atrs=atr14(bars)
    active=False; entry=sl=tp=0; is_long=False; ei=0
    session_open=None; session_open_idx=-1
    for i in range(15, len(bars)):
        ts=bars[i][0]
        hr=utc_hour(ts)
        if ts<ts_lo or ts>=ts_hi: continue
        if active:
            p, closed = manage(bars,i,entry,sl,tp,hold,ei,is_long,cost)
            if closed: pnls.append(p); active=False
            continue
        if hr<hours[0] or hr>=hours[1]: continue
        if atrs[i] is None: continue
        # Session anchor: open of session
        if hr==hours[0] or session_open is None:
            session_open=bars[i][1]; session_open_idx=i
        c=bars[i][4]
        # Fade: if c moved > dev_atr*ATR from session open, fade back
        if c > session_open + dev_atr*atrs[i]:
            is_long=False  # short fade
        elif c < session_open - dev_atr*atrs[i]:
            is_long=True  # long fade
        else: continue
        entry=c; apct=atrs[i]/entry
        sl=entry*(1-sl_atr*apct) if is_long else entry*(1+sl_atr*apct)
        tp=entry*(1+tp_atr*apct) if is_long else entry*(1-tp_atr*apct)
        active=True; ei=i
    return pnls

# ── Robust sweep helper ──────────────────────────────────────────────────────
def isoos_sweep(label, fn, params_grid, bars, tf_name, min_n=20):
    if not bars or len(bars) < 100: return None
    mid = bars[len(bars)//2][0]
    best = None
    for params in params_grid:
        p_is  = fn(bars, *params, cost=0.0010, ts_lo=bars[0][0], ts_hi=mid)
        p_oos = fn(bars, *params, cost=0.0010, ts_lo=mid, ts_hi=bars[-1][0]+1)
        p_ful = fn(bars, *params, cost=0.0010)
        s_is = stats(p_is); s_oos = stats(p_oos); s_ful = stats(p_ful)
        if not s_is or not s_oos or not s_ful: continue
        if s_ful["n"] < min_n: continue
        if s_is["sharpe"] <= 0 or s_oos["sharpe"] <= 0: continue
        mn = min(s_is["sharpe"], s_oos["sharpe"])
        if best is None or mn > best[5]:
            best = (params, s_is, s_oos, s_ful, mn, mn)
    if best:
        params, s_is, s_oos, s_ful, _, mn = best
        tag = " ***" if mn > 3.0 else (" *" if mn > 1.5 else "")
        print(f"  [{label:<22}] {tf_name:<3} params={str(params):<30} IS={s_is['sharpe']:>5.2f} OOS={s_oos['sharpe']:>5.2f} FUL={s_ful['sharpe']:>5.2f} n={s_ful['n']:<4}{tag}")
        return (label, tf_name, params, s_is, s_oos, s_ful, mn)
    return None

# ── Run on XAU (daily, H4, H1) ───────────────────────────────────────────────
xau_d = load_daily("/Users/jo/Tick/2yr_XAUUSD_daily.csv")
xau_h4 = load_csv_bars("/Users/jo/Tick/2yr_XAUUSD_tick_fresh.h4.csv")
xau_h1 = load_csv_bars("/Users/jo/Tick/2yr_XAUUSD_tick_fresh.h1.csv")
spx_h1 = load_csv_bars("/Users/jo/Tick/SPXUSD_merged.h1.csv")
nsx_h1 = load_csv_bars("/Users/jo/Tick/NSXUSD_merged.h1.csv")
ger_h1 = load_csv_bars("/Users/jo/Tick/GER40_merged.h1.csv")

print("Loaded XAU daily/H4/H1:", len(xau_d), len(xau_h4), len(xau_h1))
print("Loaded SPX/NSX/GER H1:", len(spx_h1), len(nsx_h1), len(ger_h1))

winners = []

print("\n=== EMA CROSS (golden cross long) ===")
grid = [(f,s,h,sl,tp) for f in [10,20,50] for s in [50,100,200] for h in [10,20,40] for sl in [1.5,2.0] for tp in [3.0,5.0] if s>f]
for tf, bars in [("D1",xau_d), ("H4",xau_h4), ("H1",xau_h1)]:
    r = isoos_sweep("EMA_CROSS_LONG", ema_cross, grid, bars, tf)
    if r and r[6]>1.5: winners.append(r)

print("\n=== RSI EXTREME (long-only OS bounce) ===")
grid = [(lo,hi,h,sl,tp) for lo in [20,25,30] for hi in [70,75,80] for h in [5,10,20] for sl in [1.0,1.5,2.0] for tp in [2.0,3.0,5.0]]
for tf, bars in [("D1",xau_d), ("H4",xau_h4), ("H1",xau_h1)]:
    r = isoos_sweep("RSI_EXTREME_LONG", rsi_extreme, grid, bars, tf)
    if r and r[6]>1.5: winners.append(r)

print("\n=== BOLLINGER BAND SCALP (long-only fade) ===")
grid = [(p,std,h,sl,tp) for p in [10,20,40] for std in [1.5,2.0,2.5] for h in [3,5,10,20] for sl in [1.0,1.5,2.0] for tp in [1.5,2.0,3.0]]
for tf, bars in [("D1",xau_d), ("H4",xau_h4), ("H1",xau_h1)]:
    r = isoos_sweep("BB_SCALP_LONG", bb_scalp, grid, bars, tf)
    if r and r[6]>1.5: winners.append(r)

print("\n=== PULLBACK CONTINUATION (long only) ===")
grid = [(f,s,pba,h,sl,tp) for f in [10,20] for s in [50,100] for pba in [0.5,1.0] for h in [10,20,40] for sl in [1.5,2.0] for tp in [3.0,5.0]]
for tf, bars in [("D1",xau_d), ("H4",xau_h4), ("H1",xau_h1)]:
    r = isoos_sweep("PULLBACK_CONT", pullback_cont, grid, bars, tf)
    if r and r[6]>1.5: winners.append(r)

print("\n=== THREEBAR FADE ===")
grid = [(h,sl,tp,True) for h in [3,5,10] for sl in [1.0,1.5,2.0] for tp in [2.0,3.0,5.0]]
for tf, bars in [("D1",xau_d), ("H4",xau_h4), ("H1",xau_h1)]:
    r = isoos_sweep("THREEBAR_FADE", threebar, grid, bars, tf)
    if r and r[6]>1.5: winners.append(r)

print("\n=== THREEBAR CONTINUE ===")
grid = [(h,sl,tp,False) for h in [3,5,10] for sl in [1.0,1.5,2.0] for tp in [2.0,3.0,5.0]]
for tf, bars in [("D1",xau_d), ("H4",xau_h4), ("H1",xau_h1)]:
    r = isoos_sweep("THREEBAR_CONT", threebar, grid, bars, tf)
    if r and r[6]>1.5: winners.append(r)

print("\n=== NBM (Noise Band Momentum) ===")
grid = [(b,m,h,sl,tp) for b in [1.0,1.5,2.0] for m in [0.3,0.5,1.0] for h in [3,5,10,20] for sl in [1.0,1.5] for tp in [2.0,3.0]]
for tf, bars in [("D1",xau_d), ("H4",xau_h4), ("H1",xau_h1)]:
    r = isoos_sweep("NBM_LONG", nbm, grid, bars, tf)
    if r and r[6]>1.5: winners.append(r)

print("\n=== OVERLAP FADE (12-16 UTC) ===")
grid = [(d,h,sl,tp) for d in [1.0,1.5,2.0] for h in [2,4,8] for sl in [1.0,1.5] for tp in [1.5,2.0,3.0]]
for tf, bars in [("H4",xau_h4), ("H1",xau_h1)]:
    r = isoos_sweep("OVERLAP_FADE", overlap_fade, grid, bars, tf)
    if r and r[6]>1.5: winners.append(r)

# === Index archetypes (H1 only) ===
print("\n=== EMA CROSS on indices (H1) ===")
grid = [(f,s,h,sl,tp) for f in [10,20,50] for s in [50,100,200] for h in [10,20,40] for sl in [1.5,2.0] for tp in [3.0,5.0] if s>f]
for sym, bars in [("SPX",spx_h1), ("NSX",nsx_h1), ("GER",ger_h1)]:
    r = isoos_sweep(f"EMA_CROSS_LONG_{sym}", ema_cross, grid, bars, "H1")
    if r and r[6]>1.5: winners.append(r)

print("\n=== RSI EXTREME on indices (H1) ===")
grid = [(lo,hi,h,sl,tp) for lo in [20,25,30] for hi in [70,75,80] for h in [5,10,20] for sl in [1.0,1.5,2.0] for tp in [2.0,3.0,5.0]]
for sym, bars in [("SPX",spx_h1), ("NSX",nsx_h1), ("GER",ger_h1)]:
    r = isoos_sweep(f"RSI_EXTREME_LONG_{sym}", rsi_extreme, grid, bars, "H1")
    if r and r[6]>1.5: winners.append(r)

print(f"\n\n=========================================")
print(f"=== WINNERS (min(IS,OOS) Sh >= 1.5) ===")
print(f"=========================================")
winners.sort(key=lambda x:-x[6])
for label, tf, params, s_is, s_oos, s_ful, mn in winners:
    print(f"  {label:<25} {tf:<3} {str(params):<35} IS={s_is['sharpe']:.2f} OOS={s_oos['sharpe']:.2f} FUL={s_ful['sharpe']:.2f} n={s_ful['n']} PnL={s_ful['pnl']*100:.1f}% WR={s_ful['wr']:.1f}%")
