#!/usr/bin/env python3
"""
MEGA SWEEP: every discarded engine archetype × every symbol × every timeframe.

Ranks survivors by min(IS,OOS) Sharpe with cost stress + robustness.

ARCHETYPES TESTED (each killed/disabled somewhere in main):
- EMA Cross (golden/death)
- RSI Extreme (OB/OS bounce)
- Bollinger Band Scalp
- Pullback Continuation
- Noise Band Momentum (NBM)
- Overlap Session Fade
- Stop Run Reversal
- Turtle/Donchian Break
- TSMomentum (lookback log-return)
- ThreeBar Pattern
- VWAP Reversion (session VWAP fade)
- ORB (Opening Range Breakout)
- Macro Crash (vol spike + dir)
- Trend Pullback (ADX-filtered pullback)
- StructuralEdge (Asia/Overlap persist)
- Candle Pattern (engulfing)
- Volatility Expansion (ATR contraction → break)
- Swing High/Low Break

SYMBOLS: XAUUSD (D1/H4/H1), SPXUSD H1, NSXUSD H1, GER40 H1,
         EURUSD H1, GBPUSD H1, USDJPY H1, USDCAD H1

RIGOR: IS/OOS robust (both halves positive Sh), cost 10bps default.
"""
import csv, math, os, time
from datetime import datetime, timezone
from collections import deque

t_start = time.time()

def load_csv(path):
    if not os.path.exists(path): return []
    bars=[]
    with open(path) as f:
        r=csv.reader(f); next(r,None)
        for row in r:
            try: bars.append((int(row[0]), float(row[1]), float(row[2]), float(row[3]), float(row[4])))
            except: continue
    return bars

def load_daily(path):
    if not os.path.exists(path): return []
    bars=[]
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
    cum=0; pk=0; mdd=0
    for p in pnls:
        cum+=p
        if cum>pk: pk=cum
        if pk-cum>mdd: mdd=pk-cum
    return dict(n=n,pnl=pnl,wr=wr,sharpe=sh,mdd=mdd)

def atr_series(bars, period=14):
    out=[None]*len(bars); trs=deque(maxlen=period); pc=None
    for i,(_,_,h,l,c) in enumerate(bars):
        tr=h-l
        if pc is not None: tr=max(tr,abs(h-pc),abs(l-pc))
        trs.append(tr); pc=c
        if len(trs)==period: out[i]=sum(trs)/period
    return out

def ema_series(bars, p):
    out=[None]*len(bars); a=2/(p+1); v=None
    for i,b in enumerate(bars):
        v=b[4] if v is None else a*b[4]+(1-a)*v
        if i>=p: out[i]=v
    return out

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

def utc_hour(ts): return (ts // 3600) % 24

# ── ARCHETYPE definitions (all long-only unless noted) ───────────────────────

def a_ema_cross(bars, fast_p, slow_p, hold, sl_atr, tp_atr, cost, ts_lo, ts_hi):
    pnls=[]; atrs=atr_series(bars); e_f=ema_series(bars,fast_p); e_s=ema_series(bars,slow_p)
    active=False; entry=sl=tp=0; ei=0
    prev_above=None
    for i in range(slow_p+1,len(bars)):
        ts=bars[i][0]
        if ts<ts_lo or ts>=ts_hi: continue
        if active:
            p,closed=manage(bars,i,entry,sl,tp,hold,ei,True,cost)
            if closed: pnls.append(p); active=False
            continue
        if e_f[i] is None or e_s[i] is None or atrs[i] is None: continue
        cur=e_f[i]>e_s[i]
        if prev_above is None: prev_above=cur; continue
        if (not prev_above) and cur:
            entry=bars[i][4]; apct=atrs[i]/entry
            sl=entry*(1-sl_atr*apct); tp=entry*(1+tp_atr*apct)
            active=True; ei=i
        prev_above=cur
    return pnls

def a_rsi_extreme(bars, lo_th, hi_th, hold, sl_atr, tp_atr, cost, ts_lo, ts_hi, long_only=True):
    pnls=[]; atrs=atr_series(bars)
    out=[None]*len(bars); gains=deque(maxlen=14); losses=deque(maxlen=14)
    for i in range(1,len(bars)):
        ch=bars[i][4]-bars[i-1][4]
        gains.append(max(0,ch)); losses.append(max(0,-ch))
        if len(gains)==14:
            ag=sum(gains)/14; al=sum(losses)/14
            out[i]=100 if al==0 else 100-100/(1+ag/al)
    active=False; entry=sl=tp=0; ei=0; is_long=False
    for i in range(20,len(bars)):
        ts=bars[i][0]
        if ts<ts_lo or ts>=ts_hi: continue
        if active:
            p,closed=manage(bars,i,entry,sl,tp,hold,ei,is_long,cost)
            if closed: pnls.append(p); active=False
            continue
        if out[i] is None or atrs[i] is None: continue
        if out[i]<lo_th: is_long=True
        elif out[i]>hi_th and not long_only: is_long=False
        else: continue
        entry=bars[i][4]; apct=atrs[i]/entry
        sl=entry*(1-sl_atr*apct) if is_long else entry*(1+sl_atr*apct)
        tp=entry*(1+tp_atr*apct) if is_long else entry*(1-tp_atr*apct)
        active=True; ei=i
    return pnls

def a_bb_scalp(bars, period, std_mult, hold, sl_atr, tp_atr, cost, ts_lo, ts_hi):
    pnls=[]; atrs=atr_series(bars); closes=[b[4] for b in bars]
    active=False; entry=sl=tp=0; ei=0
    for i in range(period+1,len(bars)):
        ts=bars[i][0]
        if ts<ts_lo or ts>=ts_hi: continue
        if active:
            p,closed=manage(bars,i,entry,sl,tp,hold,ei,True,cost)
            if closed: pnls.append(p); active=False
            continue
        wd=closes[i-period:i]; m=sum(wd)/period; sd=math.sqrt(sum((x-m)**2 for x in wd)/(period-1))
        if sd<=0 or atrs[i] is None: continue
        lower=m-std_mult*sd
        if bars[i][4]<lower:
            entry=bars[i][4]; apct=atrs[i]/entry
            sl=entry*(1-sl_atr*apct); tp=entry*(1+tp_atr*apct)
            active=True; ei=i
    return pnls

def a_pullback(bars, fast_p, slow_p, pb_atr, hold, sl_atr, tp_atr, cost, ts_lo, ts_hi):
    pnls=[]; atrs=atr_series(bars); ef=ema_series(bars,fast_p); es=ema_series(bars,slow_p)
    active=False; entry=sl=tp=0; ei=0
    for i in range(slow_p+1,len(bars)):
        ts=bars[i][0]
        if ts<ts_lo or ts>=ts_hi: continue
        if active:
            p,closed=manage(bars,i,entry,sl,tp,hold,ei,True,cost)
            if closed: pnls.append(p); active=False
            continue
        if ef[i] is None or es[i] is None or atrs[i] is None: continue
        if ef[i]<=es[i]: continue
        if bars[i][3]>ef[i]+pb_atr*atrs[i]: continue
        if bars[i][4]<ef[i]: continue
        entry=bars[i][4]; apct=atrs[i]/entry
        sl=entry*(1-sl_atr*apct); tp=entry*(1+tp_atr*apct)
        active=True; ei=i
    return pnls

def a_nbm(bars, band_atr, mom_atr, hold, sl_atr, tp_atr, cost, ts_lo, ts_hi):
    pnls=[]; atrs=atr_series(bars); ma=ema_series(bars,20)
    active=False; entry=sl=tp=0; ei=0
    for i in range(25,len(bars)):
        ts=bars[i][0]
        if ts<ts_lo or ts>=ts_hi: continue
        if active:
            p,closed=manage(bars,i,entry,sl,tp,hold,ei,True,cost)
            if closed: pnls.append(p); active=False
            continue
        if ma[i] is None or atrs[i] is None: continue
        upper=ma[i]+band_atr*atrs[i]+mom_atr*atrs[i]
        if bars[i][4]>upper:
            entry=bars[i][4]; apct=atrs[i]/entry
            sl=entry*(1-sl_atr*apct); tp=entry*(1+tp_atr*apct)
            active=True; ei=i
    return pnls

def a_turtle(bars, lb, hold, sl_atr, tp_atr, cost, ts_lo, ts_hi):
    pnls=[]; atrs=atr_series(bars)
    active=False; entry=sl=tp=0; ei=0
    for i in range(lb,len(bars)):
        ts=bars[i][0]
        if ts<ts_lo or ts>=ts_hi: continue
        if active:
            p,closed=manage(bars,i,entry,sl,tp,hold,ei,True,cost)
            if closed: pnls.append(p); active=False
            continue
        if atrs[i] is None: continue
        ph=max(b[2] for b in bars[i-lb:i])
        if bars[i][4]>ph:
            entry=bars[i][4]; apct=atrs[i]/entry
            sl=entry*(1-sl_atr*apct); tp=entry*(1+tp_atr*apct)
            active=True; ei=i
    return pnls

def a_stoprun(bars, lb, hold, sl_atr, tp_atr, cost, ts_lo, ts_hi):
    pnls=[]; atrs=atr_series(bars)
    active=False; entry=sl=tp=0; ei=0
    for i in range(lb,len(bars)):
        ts=bars[i][0]
        if ts<ts_lo or ts>=ts_hi: continue
        if active:
            p,closed=manage(bars,i,entry,sl,tp,hold,ei,True,cost)
            if closed: pnls.append(p); active=False
            continue
        if atrs[i] is None: continue
        pl=min(b[3] for b in bars[i-lb:i])
        if bars[i][3]<pl and bars[i][4]>pl:
            entry=bars[i][4]; apct=atrs[i]/entry
            sl=entry*(1-sl_atr*apct); tp=entry*(1+tp_atr*apct)
            active=True; ei=i
    return pnls

def a_tsmom(bars, lb, hold, sl_atr, tp_atr, cost, ts_lo, ts_hi, min_mom=0.005):
    pnls=[]; atrs=atr_series(bars)
    active=False; entry=sl=tp=0; ei=0
    for i in range(lb,len(bars)):
        ts=bars[i][0]
        if ts<ts_lo or ts>=ts_hi: continue
        if active:
            p,closed=manage(bars,i,entry,sl,tp,hold,ei,True,cost)
            if closed: pnls.append(p); active=False
            continue
        if atrs[i] is None: continue
        mom=math.log(bars[i][4]/bars[i-lb][4])
        if mom<min_mom: continue
        entry=bars[i][4]; apct=atrs[i]/entry
        sl=entry*(1-sl_atr*apct); tp=entry*(1+tp_atr*apct)
        active=True; ei=i
    return pnls

def a_overlap_fade(bars, dev_atr, hold, sl_atr, tp_atr, cost, ts_lo, ts_hi, hr_lo=12, hr_hi=16):
    pnls=[]; atrs=atr_series(bars)
    active=False; entry=sl=tp=0; ei=0; is_long=False
    session_open=None; prev_hr=-1
    for i in range(15,len(bars)):
        ts=bars[i][0]
        hr=utc_hour(ts)
        if ts<ts_lo or ts>=ts_hi: continue
        if active:
            p,closed=manage(bars,i,entry,sl,tp,hold,ei,is_long,cost)
            if closed: pnls.append(p); active=False
            continue
        if hr<hr_lo or hr>=hr_hi: prev_hr=hr; continue
        if session_open is None or prev_hr<hr_lo: session_open=bars[i][1]
        prev_hr=hr
        if atrs[i] is None: continue
        c=bars[i][4]
        dev=c-session_open
        if dev>dev_atr*atrs[i]:
            is_long=False
        elif dev<-dev_atr*atrs[i]:
            is_long=True
        else: continue
        entry=c; apct=atrs[i]/entry
        sl=entry*(1-sl_atr*apct) if is_long else entry*(1+sl_atr*apct)
        tp=entry*(1+tp_atr*apct) if is_long else entry*(1-tp_atr*apct)
        active=True; ei=i
    return pnls

def a_volexp(bars, contract_ratio, hold, sl_atr, tp_atr, cost, ts_lo, ts_hi):
    """ATR contraction (short ATR < ratio * long ATR) then close > prev high = long."""
    pnls=[]; atr5=[None]*len(bars); atr20=[None]*len(bars)
    trs=deque(maxlen=20); trs5=deque(maxlen=5); pc=None
    for i,(_,_,h,l,c) in enumerate(bars):
        tr=h-l
        if pc is not None: tr=max(tr,abs(h-pc),abs(l-pc))
        trs.append(tr); trs5.append(tr); pc=c
        if len(trs5)==5: atr5[i]=sum(trs5)/5
        if len(trs)==20: atr20[i]=sum(trs)/20
    active=False; entry=sl=tp=0; ei=0
    for i in range(21,len(bars)):
        ts=bars[i][0]
        if ts<ts_lo or ts>=ts_hi: continue
        if active:
            p,closed=manage(bars,i,entry,sl,tp,hold,ei,True,cost)
            if closed: pnls.append(p); active=False
            continue
        if atr5[i] is None or atr20[i] is None: continue
        if atr5[i]>=contract_ratio*atr20[i]: continue
        if bars[i][4]>bars[i-1][2]:
            entry=bars[i][4]; apct=atr20[i]/entry
            sl=entry*(1-sl_atr*apct); tp=entry*(1+tp_atr*apct)
            active=True; ei=i
    return pnls

def a_orb(bars, range_hours, hold, sl_atr, tp_atr, cost, ts_lo, ts_hi, session_start=7):
    """Pre-session range break: high break = long, hold until SL/TP/timeout."""
    pnls=[]; atrs=atr_series(bars)
    active=False; entry=sl=tp=0; ei=0
    cur_day=-1; r_hi=0; r_lo=1e18; fired=False
    for i in range(15,len(bars)):
        ts=bars[i][0]; hr=utc_hour(ts); day=ts//86400
        if ts<ts_lo or ts>=ts_hi: continue
        if day!=cur_day: cur_day=day; r_hi=0; r_lo=1e18; fired=False
        if active:
            p,closed=manage(bars,i,entry,sl,tp,hold,ei,True,cost)
            if closed: pnls.append(p); active=False
            continue
        if hr<session_start:
            if bars[i][2]>r_hi: r_hi=bars[i][2]
            if bars[i][3]<r_lo: r_lo=bars[i][3]
        elif hr>=session_start and hr<session_start+range_hours and not fired:
            if atrs[i] is None or r_hi<=0 or r_lo>=1e17: continue
            if bars[i][4]>r_hi:
                fired=True
                entry=bars[i][4]; apct=atrs[i]/entry
                sl=entry*(1-sl_atr*apct); tp=entry*(1+tp_atr*apct)
                active=True; ei=i
    return pnls

def a_engulfing(bars, hold, sl_atr, tp_atr, cost, ts_lo, ts_hi):
    """Bullish engulfing: prev bar bearish, current bullish + bigger body, takes out prev low."""
    pnls=[]; atrs=atr_series(bars)
    active=False; entry=sl=tp=0; ei=0
    for i in range(15,len(bars)):
        ts=bars[i][0]
        if ts<ts_lo or ts>=ts_hi: continue
        if active:
            p,closed=manage(bars,i,entry,sl,tp,hold,ei,True,cost)
            if closed: pnls.append(p); active=False
            continue
        if atrs[i] is None: continue
        prev=bars[i-1]; cur=bars[i]
        prev_bear = prev[4]<prev[1]
        cur_bull = cur[4]>cur[1]
        prev_body = abs(prev[4]-prev[1])
        cur_body = abs(cur[4]-cur[1])
        if not (prev_bear and cur_bull): continue
        if cur_body < 1.5*prev_body: continue
        if cur[1]>prev[4] or cur[4]<prev[1]: continue  # body engulf check
        entry=cur[4]; apct=atrs[i]/entry
        sl=entry*(1-sl_atr*apct); tp=entry*(1+tp_atr*apct)
        active=True; ei=i
    return pnls

def a_swing_break(bars, lb, hold, sl_atr, tp_atr, cost, ts_lo, ts_hi):
    """Break of last N-bar high but only when bar is making higher highs and higher lows."""
    pnls=[]; atrs=atr_series(bars)
    active=False; entry=sl=tp=0; ei=0
    for i in range(lb+2,len(bars)):
        ts=bars[i][0]
        if ts<ts_lo or ts>=ts_hi: continue
        if active:
            p,closed=manage(bars,i,entry,sl,tp,hold,ei,True,cost)
            if closed: pnls.append(p); active=False
            continue
        if atrs[i] is None: continue
        # Higher highs+lows over last 3 bars
        if not (bars[i-2][2]<bars[i-1][2]<bars[i][2] and bars[i-2][3]<bars[i-1][3]<bars[i][3]): continue
        ph=max(b[2] for b in bars[i-lb:i])
        if bars[i][4]>ph:
            entry=bars[i][4]; apct=atrs[i]/entry
            sl=entry*(1-sl_atr*apct); tp=entry*(1+tp_atr*apct)
            active=True; ei=i
    return pnls

# ── Sweep helper ─────────────────────────────────────────────────────────────
def sweep(label, fn, params_grid, bars, tf, sym, results, min_n=15, min_sh=1.5, cost=0.0010):
    if not bars or len(bars)<150: return
    mid=bars[len(bars)//2][0]
    best=None
    for params in params_grid:
        try:
            p_is=fn(bars, *params, cost, bars[0][0], mid)
            p_oos=fn(bars, *params, cost, mid, bars[-1][0]+1)
            p_ful=fn(bars, *params, cost, 0, 10**12)
        except: continue
        s_is=stats(p_is); s_oos=stats(p_oos); s_ful=stats(p_ful)
        if not s_is or not s_oos or not s_ful: continue
        if s_ful["n"]<min_n: continue
        if s_is["sharpe"]<=0 or s_oos["sharpe"]<=0: continue
        mn=min(s_is["sharpe"],s_oos["sharpe"])
        if best is None or mn>best[5]:
            best=(params, s_is, s_oos, s_ful, mn, mn)
    if best:
        params,s_is,s_oos,s_ful,_,mn=best
        if mn>=min_sh:
            results.append((label, sym, tf, params, s_is, s_oos, s_ful, mn))
            print(f"  [{label:<18}] {sym:<7} {tf:<3} min_sh={mn:.2f} IS={s_is['sharpe']:.2f} OOS={s_oos['sharpe']:.2f} FUL={s_ful['sharpe']:.2f} n={s_ful['n']} PnL={s_ful['pnl']*100:.1f}% WR={s_ful['wr']:.1f}% mdd={s_ful['mdd']*100:.1f}%")

# ── Symbol bars ──────────────────────────────────────────────────────────────
print("Loading data...")
SYMS = {
    "XAU_D1":  load_daily("/Users/jo/Tick/2yr_XAUUSD_daily.csv"),
    "XAU_H4":  load_csv("/Users/jo/Tick/2yr_XAUUSD_tick_fresh.h4.csv"),
    "XAU_H1":  load_csv("/Users/jo/Tick/2yr_XAUUSD_tick_fresh.h1.csv"),
    "SPX_H1":  load_csv("/Users/jo/Tick/SPXUSD_merged.h1.csv"),
    "SPX_H4":  load_csv("/Users/jo/Tick/SPXUSD_merged.h4.csv"),
    "NSX_H1":  load_csv("/Users/jo/Tick/NSXUSD_merged.h1.csv"),
    "NSX_H4":  load_csv("/Users/jo/Tick/NSXUSD_merged.h4.csv"),
    "GER_H1":  load_csv("/Users/jo/Tick/GER40_merged.h1.csv"),
    "GER_H4":  load_csv("/Users/jo/Tick/GER40_merged.h4.csv"),
    "EUR_H1":  load_csv("/Users/jo/Tick/EURUSD_merged.h1.csv"),
    "GBP_H1":  load_csv("/Users/jo/Tick/GBPUSD_merged.h1.csv"),
    "JPY_H1":  load_csv("/Users/jo/Tick/USDJPY_merged.h1.csv"),
    "JPY_H4":  load_csv("/Users/jo/Tick/USDJPY_merged.h4.csv"),
    "CAD_H1":  load_csv("/Users/jo/Tick/USDCAD_merged.h1.csv"),
}
for k,v in SYMS.items(): print(f"  {k}: {len(v)} bars")

results = []

# ── Param grids ──────────────────────────────────────────────────────────────
GR_EMA   = [(f,s,h,sl,tp) for f in [10,20,50] for s in [50,100,200] for h in [10,20,40] for sl in [1.5,2.0] for tp in [3.0,5.0] if s>f]
GR_RSI   = [(lo,hi,h,sl,tp) for lo in [25,30] for hi in [70,75] for h in [5,10,20] for sl in [1.0,1.5,2.0] for tp in [2.0,3.0,5.0]]
GR_BB    = [(p,std,h,sl,tp) for p in [10,20] for std in [1.5,2.0,2.5] for h in [5,10,20] for sl in [1.0,1.5,2.0] for tp in [1.5,2.0,3.0]]
GR_PB    = [(f,s,pba,h,sl,tp) for f in [10,20] for s in [50,100] for pba in [0.5,1.0] for h in [10,20,40] for sl in [1.5,2.0] for tp in [3.0,5.0]]
GR_NBM   = [(b,m,h,sl,tp) for b in [1.0,1.5,2.0] for m in [0.3,0.5,1.0] for h in [5,10,20] for sl in [1.0,1.5,2.0] for tp in [2.0,3.0,5.0]]
GR_TURT  = [(lb,h,sl,tp) for lb in [10,20,40] for h in [5,10,20] for sl in [1.5,2.0] for tp in [3.0,5.0,8.0]]
GR_STOP  = [(lb,h,sl,tp) for lb in [5,10,20] for h in [10,20] for sl in [1.0,1.5,2.0] for tp in [2.0,3.0,5.0]]
GR_TSMOM = [(lb,h,sl,tp) for lb in [3,5,10,20] for h in [5,10,20,40] for sl in [1.0,1.5,2.0] for tp in [3.0,5.0,8.0]]
GR_OVR   = [(d,h,sl,tp) for d in [1.0,1.5,2.0] for h in [3,5,10] for sl in [1.0,1.5] for tp in [1.5,2.0,3.0]]
GR_VEX   = [(cr,h,sl,tp) for cr in [0.6,0.7,0.8] for h in [5,10,20] for sl in [1.5,2.0] for tp in [3.0,5.0]]
GR_ORB   = [(rh,h,sl,tp) for rh in [4,8,12] for h in [3,5,10] for sl in [1.0,1.5,2.0] for tp in [2.0,3.0,5.0]]
GR_ENG   = [(h,sl,tp) for h in [3,5,10,20] for sl in [1.0,1.5,2.0] for tp in [2.0,3.0,5.0]]
GR_SWG   = [(lb,h,sl,tp) for lb in [5,10,20] for h in [5,10,20] for sl in [1.0,1.5,2.0] for tp in [2.0,3.0,5.0]]

print(f"\n=== Running mega sweep (12 archetypes × 14 symbol/TF combos) ===")
print(f"Cost 10bps, IS/OOS robust filter, min n=15, min Sh=1.5\n")

for sym, bars in SYMS.items():
    tf = sym.split("_")[1]; sname = sym.split("_")[0]
    if not bars: continue
    sweep("EMA_CROSS",   a_ema_cross, GR_EMA,   bars, tf, sname, results)
    sweep("RSI_EXTREME", a_rsi_extreme, GR_RSI, bars, tf, sname, results)
    sweep("BB_SCALP",    a_bb_scalp,  GR_BB,    bars, tf, sname, results)
    sweep("PULLBACK",    a_pullback,  GR_PB,    bars, tf, sname, results)
    sweep("NBM",         a_nbm,       GR_NBM,   bars, tf, sname, results)
    sweep("TURTLE",      a_turtle,    GR_TURT,  bars, tf, sname, results)
    sweep("STOPRUN",     a_stoprun,   GR_STOP,  bars, tf, sname, results)
    sweep("TSMOM",       a_tsmom,     GR_TSMOM, bars, tf, sname, results)
    sweep("OVERLAP_FADE",a_overlap_fade, GR_OVR, bars, tf, sname, results)
    sweep("VOL_EXP",     a_volexp,    GR_VEX,   bars, tf, sname, results)
    sweep("ORB",         a_orb,       GR_ORB,   bars, tf, sname, results)
    sweep("ENGULFING",   a_engulfing, GR_ENG,   bars, tf, sname, results)
    sweep("SWING_BREAK", a_swing_break, GR_SWG, bars, tf, sname, results)

print(f"\n=== FINAL RANKING (sorted by min(IS,OOS) Sharpe) ===")
print(f"{'Rank':>4} {'Archetype':<14} {'Sym':<5} {'TF':<3} {'IS_Sh':>6} {'OOS_Sh':>6} {'FUL_Sh':>6} {'n':>4} {'PnL%':>6} {'WR%':>5} {'MDD%':>5}")
print("-"*100)
results.sort(key=lambda x:-x[7])
for i,(label,sym,tf,params,s_is,s_oos,s_ful,mn) in enumerate(results, 1):
    print(f"{i:>4} {label:<14} {sym:<5} {tf:<3} {s_is['sharpe']:>6.2f} {s_oos['sharpe']:>6.2f} {s_ful['sharpe']:>6.2f} {s_ful['n']:>4} {s_ful['pnl']*100:>5.1f}% {s_ful['wr']:>4.1f}% {s_ful['mdd']*100:>4.1f}%")

print(f"\n=== STATS ===")
print(f"Total winners: {len(results)}")
print(f"  min_sh >= 3.0: {sum(1 for r in results if r[7]>=3.0)}")
print(f"  min_sh >= 5.0: {sum(1 for r in results if r[7]>=5.0)}")
by_arch = {}
for r in results: by_arch[r[0]] = by_arch.get(r[0],0)+1
print(f"  by archetype: {by_arch}")
by_sym = {}
for r in results: by_sym[r[1]] = by_sym.get(r[1],0)+1
print(f"  by symbol: {by_sym}")
print(f"\nTotal elapsed: {time.time()-t_start:.0f}s")
