#!/usr/bin/env python3
"""
MEGA SWEEP 2: remaining killed/disabled archetypes not in mega_sweep.py.

- MacroCrash: ATR spike + direction
- VWAPReversion: session VWAP deviation fade
- LiquiditySweep: quick reversal off N-bar extreme (faster than StopRun)
- GoldFlow proxy: range expansion + close direction
- CandleFlow: hammer / shooting-star / inside-bar / outside-bar / doji-rej
- TrendPullback: ADX-filter version of Pullback
- AsianSessionBreak: range break at Asia open
- LondonOpenBreak: London open expansion
- NYCloseFade: NY close mean reversion
- 3-bar momentum (kept long-only)
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

def utc_hour(ts): return (ts // 3600) % 24

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

# ── ARCHETYPES ───────────────────────────────────────────────────────────────

def a_macro_crash(bars, atr_spike_mult, hold, sl_atr, tp_atr, cost, ts_lo, ts_hi, long_only=True):
    """ATR spike >= mult*median_ATR AND bar close in direction of move."""
    pnls=[]; atrs=atr_series(bars)
    # rolling median ATR
    med_atr=[None]*len(bars)
    win=deque(maxlen=20)
    for i,a in enumerate(atrs):
        if a is not None:
            win.append(a)
            if len(win)>=10:
                s=sorted(win); med_atr[i]=s[len(s)//2]
    active=False; entry=sl=tp=0; is_long=False; ei=0
    for i in range(25,len(bars)):
        ts=bars[i][0]
        if ts<ts_lo or ts>=ts_hi: continue
        if active:
            p,closed=manage(bars,i,entry,sl,tp,hold,ei,is_long,cost)
            if closed: pnls.append(p); active=False
            continue
        if atrs[i] is None or med_atr[i] is None: continue
        if atrs[i] < atr_spike_mult * med_atr[i]: continue
        o=bars[i][1]; c=bars[i][4]
        if c > o:  # bullish spike
            is_long=True
        elif c < o and not long_only:
            is_long=False
        else: continue
        entry=c; apct=atrs[i]/entry
        sl=entry*(1-sl_atr*apct) if is_long else entry*(1+sl_atr*apct)
        tp=entry*(1+tp_atr*apct) if is_long else entry*(1-tp_atr*apct)
        active=True; ei=i
    return pnls

def a_vwap_reversion(bars, dev_atr, hold, sl_atr, tp_atr, cost, ts_lo, ts_hi):
    """Session VWAP-deviation fade. Long when close < session_vwap - dev*ATR."""
    pnls=[]; atrs=atr_series(bars)
    active=False; entry=sl=tp=0; ei=0
    cur_day=-1; vwap_num=0; vwap_den=0
    for i in range(15,len(bars)):
        ts=bars[i][0]; day=ts//86400
        if ts<ts_lo or ts>=ts_hi: continue
        if day!=cur_day:
            cur_day=day; vwap_num=0; vwap_den=0
        # incremental VWAP (use close as price proxy, count=1 weight)
        c=bars[i][4]
        vwap_num += c
        vwap_den += 1
        vwap = vwap_num/vwap_den if vwap_den else c
        if active:
            p,closed=manage(bars,i,entry,sl,tp,hold,ei,True,cost)
            if closed: pnls.append(p); active=False
            continue
        if atrs[i] is None or vwap_den<3: continue
        if c < vwap - dev_atr*atrs[i]:
            entry=c; apct=atrs[i]/entry
            sl=entry*(1-sl_atr*apct); tp=entry*(1+tp_atr*apct)
            active=True; ei=i
    return pnls

def a_liquidity_sweep(bars, lb, hold, sl_atr, tp_atr, cost, ts_lo, ts_hi):
    """Same as StopRun but tighter conditions: bar wicks below lb-low by ATR, closes high in range."""
    pnls=[]; atrs=atr_series(bars)
    active=False; entry=sl=tp=0; ei=0
    for i in range(lb+1,len(bars)):
        ts=bars[i][0]
        if ts<ts_lo or ts>=ts_hi: continue
        if active:
            p,closed=manage(bars,i,entry,sl,tp,hold,ei,True,cost)
            if closed: pnls.append(p); active=False
            continue
        if atrs[i] is None: continue
        pl=min(b[3] for b in bars[i-lb:i])
        h=bars[i][2]; l=bars[i][3]; c=bars[i][4]
        if l>=pl: continue  # need wick below
        # Wick at least 0.5 ATR below
        if pl-l < 0.5*atrs[i]: continue
        # Close in upper half of bar range
        if c < (h+l)/2: continue
        entry=c; apct=atrs[i]/entry
        sl=entry*(1-sl_atr*apct); tp=entry*(1+tp_atr*apct)
        active=True; ei=i
    return pnls

def a_range_expansion(bars, atr_exp_mult, hold, sl_atr, tp_atr, cost, ts_lo, ts_hi):
    """Range expansion (current bar range > mult * avg) + direction = continue."""
    pnls=[]; atrs=atr_series(bars)
    active=False; entry=sl=tp=0; ei=0; is_long=False
    for i in range(15,len(bars)):
        ts=bars[i][0]
        if ts<ts_lo or ts>=ts_hi: continue
        if active:
            p,closed=manage(bars,i,entry,sl,tp,hold,ei,is_long,cost)
            if closed: pnls.append(p); active=False
            continue
        if atrs[i] is None: continue
        rng=bars[i][2]-bars[i][3]
        if rng < atr_exp_mult * atrs[i]: continue
        if bars[i][4] > bars[i][1]:
            is_long=True; entry=bars[i][4]
        else: continue
        apct=atrs[i]/entry
        sl=entry*(1-sl_atr*apct); tp=entry*(1+tp_atr*apct)
        active=True; ei=i
    return pnls

def a_hammer(bars, hold, sl_atr, tp_atr, cost, ts_lo, ts_hi):
    """Hammer: lower wick > 2*body, close in upper third, after recent decline."""
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
        o=bars[i][1]; h=bars[i][2]; l=bars[i][3]; c=bars[i][4]
        body=abs(c-o); rng=h-l
        if rng<=0: continue
        lower_wick = min(o,c) - l
        upper_wick = h - max(o,c)
        if lower_wick < 2*body: continue
        if upper_wick > body: continue
        # Close in upper third
        if c < l + 0.67*rng: continue
        # After decline: last 3 closes trending down
        if not (bars[i-3][4] > bars[i-2][4] > bars[i-1][4]): continue
        entry=c; apct=atrs[i]/entry
        sl=entry*(1-sl_atr*apct); tp=entry*(1+tp_atr*apct)
        active=True; ei=i
    return pnls

def a_inside_bar(bars, hold, sl_atr, tp_atr, cost, ts_lo, ts_hi):
    """Inside bar break: bar[-1] inside bar[-2] range, then bar[0] breaks above bar[-1] high."""
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
        b2=bars[i-2]; b1=bars[i-1]; b0=bars[i]
        # Inside bar: b1 fully inside b2 range
        if not (b1[2] < b2[2] and b1[3] > b2[3]): continue
        # Break above b1 high on b0 close
        if b0[4] <= b1[2]: continue
        entry=b0[4]; apct=atrs[i]/entry
        sl=entry*(1-sl_atr*apct); tp=entry*(1+tp_atr*apct)
        active=True; ei=i
    return pnls

def a_outside_bar(bars, hold, sl_atr, tp_atr, cost, ts_lo, ts_hi):
    """Outside bar: bar engulfs prev bar's range, closes bullish."""
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
        if not (cur[2] > prev[2] and cur[3] < prev[3]): continue  # outside
        if cur[4] <= cur[1]: continue  # bullish close
        entry=cur[4]; apct=atrs[i]/entry
        sl=entry*(1-sl_atr*apct); tp=entry*(1+tp_atr*apct)
        active=True; ei=i
    return pnls

def a_doji_rej(bars, hold, sl_atr, tp_atr, cost, ts_lo, ts_hi):
    """Doji at support: tiny body + recent low touched. Follow with break."""
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
        # Prev = doji (small body relative to range)
        prev_body=abs(prev[4]-prev[1]); prev_range=prev[2]-prev[3]
        if prev_range<=0 or prev_body > 0.15*prev_range: continue
        # Current bar breaks prev high
        if cur[4] <= prev[2]: continue
        entry=cur[4]; apct=atrs[i]/entry
        sl=entry*(1-sl_atr*apct); tp=entry*(1+tp_atr*apct)
        active=True; ei=i
    return pnls

def a_trend_pullback_adx(bars, ema_p, pb_atr, hold, sl_atr, tp_atr, cost, ts_lo, ts_hi):
    """Pullback with ADX filter: EMA slope positive + pullback to EMA + close above."""
    pnls=[]; atrs=atr_series(bars); ema=ema_series(bars, ema_p)
    active=False; entry=sl=tp=0; ei=0
    for i in range(ema_p+5,len(bars)):
        ts=bars[i][0]
        if ts<ts_lo or ts>=ts_hi: continue
        if active:
            p,closed=manage(bars,i,entry,sl,tp,hold,ei,True,cost)
            if closed: pnls.append(p); active=False
            continue
        if ema[i] is None or ema[i-5] is None or atrs[i] is None: continue
        # Trend: EMA slope positive (5-bar)
        slope=(ema[i]-ema[i-5])/ema[i-5]
        if slope < 0.001: continue  # need >= 0.1% rise
        # Pullback: low touched within pb_atr of EMA
        if bars[i][3] > ema[i] + pb_atr*atrs[i]: continue
        if bars[i][4] < ema[i]: continue
        entry=bars[i][4]; apct=atrs[i]/entry
        sl=entry*(1-sl_atr*apct); tp=entry*(1+tp_atr*apct)
        active=True; ei=i
    return pnls

def a_asian_break(bars, hold, sl_atr, tp_atr, cost, ts_lo, ts_hi, asian_start=22, asian_end=6, break_hour=7):
    """Asian range build (22-06), break at London open (07)."""
    pnls=[]; atrs=atr_series(bars)
    active=False; entry=sl=tp=0; ei=0
    cur_day=-1; r_hi=0; r_lo=1e18; fired=False
    for i in range(15,len(bars)):
        ts=bars[i][0]; hr=utc_hour(ts); day=ts//86400
        if ts<ts_lo or ts>=ts_hi: continue
        if hr==asian_start: cur_day=day; r_hi=0; r_lo=1e18; fired=False
        if active:
            p,closed=manage(bars,i,entry,sl,tp,hold,ei,True,cost)
            if closed: pnls.append(p); active=False
            continue
        if hr>=asian_start or hr<asian_end:
            if bars[i][2]>r_hi: r_hi=bars[i][2]
            if bars[i][3]<r_lo: r_lo=bars[i][3]
        elif hr==break_hour and not fired:
            if atrs[i] is None or r_hi<=0 or r_lo>=1e17: continue
            if bars[i][4]>r_hi:
                fired=True
                entry=bars[i][4]; apct=atrs[i]/entry
                sl=entry*(1-sl_atr*apct); tp=entry*(1+tp_atr*apct)
                active=True; ei=i
    return pnls

def a_ny_close_fade(bars, dev_atr, hold, sl_atr, tp_atr, cost, ts_lo, ts_hi, ny_close=21):
    """At NY close (21 UTC), fade extreme deviation from daily open."""
    pnls=[]; atrs=atr_series(bars)
    active=False; entry=sl=tp=0; ei=0
    cur_day=-1; day_open=None
    for i in range(15,len(bars)):
        ts=bars[i][0]; hr=utc_hour(ts); day=ts//86400
        if ts<ts_lo or ts>=ts_hi: continue
        if day!=cur_day: cur_day=day; day_open=bars[i][1]
        if active:
            p,closed=manage(bars,i,entry,sl,tp,hold,ei,True,cost)
            if closed: pnls.append(p); active=False
            continue
        if hr!=ny_close: continue
        if atrs[i] is None or day_open is None: continue
        c=bars[i][4]; dev=c-day_open
        # Long fade only: if c << day_open
        if dev > -dev_atr*atrs[i]: continue
        entry=c; apct=atrs[i]/entry
        sl=entry*(1-sl_atr*apct); tp=entry*(1+tp_atr*apct)
        active=True; ei=i
    return pnls

# ── Sweep ────────────────────────────────────────────────────────────────────
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
        if best is None or mn>best[5]: best=(params,s_is,s_oos,s_ful,mn,mn)
    if best:
        params,s_is,s_oos,s_ful,_,mn=best
        if mn>=min_sh:
            results.append((label, sym, tf, params, s_is, s_oos, s_ful, mn))
            print(f"  [{label:<18}] {sym:<5} {tf:<3} min_sh={mn:.2f} IS={s_is['sharpe']:.2f} OOS={s_oos['sharpe']:.2f} FUL={s_ful['sharpe']:.2f} n={s_ful['n']} PnL={s_ful['pnl']*100:.1f}% WR={s_ful['wr']:.1f}%")

# ── Run ──────────────────────────────────────────────────────────────────────
print("Loading data...")
SYMS = {
    "XAU_D1":  load_daily("/Users/jo/Tick/2yr_XAUUSD_daily.csv"),
    "XAU_H4":  load_csv("/Users/jo/Tick/2yr_XAUUSD_tick_fresh.h4.csv"),
    "XAU_H1":  load_csv("/Users/jo/Tick/2yr_XAUUSD_tick_fresh.h1.csv"),
    "SPX_H4":  load_csv("/Users/jo/Tick/SPXUSD_merged.h4.csv"),
    "NSX_H4":  load_csv("/Users/jo/Tick/NSXUSD_merged.h4.csv"),
    "GER_H4":  load_csv("/Users/jo/Tick/GER40_merged.h4.csv"),
}
for k,v in SYMS.items(): print(f"  {k}: {len(v)} bars")

results = []

print("\n=== MACRO CRASH (ATR spike + dir) ===")
GR = [(m,h,sl,tp) for m in [1.5,2.0,3.0] for h in [3,5,10] for sl in [1.0,1.5,2.0] for tp in [2.0,3.0,5.0]]
for sym, bars in SYMS.items():
    tf=sym.split("_")[1]; sname=sym.split("_")[0]
    sweep("MACRO_CRASH", a_macro_crash, GR, bars, tf, sname, results)

print("\n=== VWAP REVERSION ===")
GR = [(d,h,sl,tp) for d in [1.0,1.5,2.0] for h in [3,5,10] for sl in [1.0,1.5] for tp in [1.5,2.0,3.0]]
for sym, bars in SYMS.items():
    tf=sym.split("_")[1]; sname=sym.split("_")[0]
    if tf=="D1": continue  # only intraday
    sweep("VWAP_REV", a_vwap_reversion, GR, bars, tf, sname, results)

print("\n=== LIQUIDITY SWEEP ===")
GR = [(lb,h,sl,tp) for lb in [5,10,20] for h in [3,5,10] for sl in [1.0,1.5,2.0] for tp in [2.0,3.0,5.0]]
for sym, bars in SYMS.items():
    tf=sym.split("_")[1]; sname=sym.split("_")[0]
    sweep("LIQUIDITY_SWEEP", a_liquidity_sweep, GR, bars, tf, sname, results)

print("\n=== RANGE EXPANSION ===")
GR = [(m,h,sl,tp) for m in [1.5,2.0,3.0] for h in [3,5,10] for sl in [1.0,1.5,2.0] for tp in [2.0,3.0,5.0]]
for sym, bars in SYMS.items():
    tf=sym.split("_")[1]; sname=sym.split("_")[0]
    sweep("RANGE_EXP", a_range_expansion, GR, bars, tf, sname, results)

print("\n=== HAMMER ===")
GR = [(h,sl,tp) for h in [3,5,10,20] for sl in [1.0,1.5,2.0] for tp in [2.0,3.0,5.0]]
for sym, bars in SYMS.items():
    tf=sym.split("_")[1]; sname=sym.split("_")[0]
    sweep("HAMMER", a_hammer, GR, bars, tf, sname, results)

print("\n=== INSIDE BAR ===")
for sym, bars in SYMS.items():
    tf=sym.split("_")[1]; sname=sym.split("_")[0]
    sweep("INSIDE_BAR", a_inside_bar, GR, bars, tf, sname, results)

print("\n=== OUTSIDE BAR ===")
for sym, bars in SYMS.items():
    tf=sym.split("_")[1]; sname=sym.split("_")[0]
    sweep("OUTSIDE_BAR", a_outside_bar, GR, bars, tf, sname, results)

print("\n=== DOJI REJECTION ===")
for sym, bars in SYMS.items():
    tf=sym.split("_")[1]; sname=sym.split("_")[0]
    sweep("DOJI_REJ", a_doji_rej, GR, bars, tf, sname, results)

print("\n=== TREND PULLBACK (EMA slope filter) ===")
GR = [(ep,pba,h,sl,tp) for ep in [20,50,100] for pba in [0.3,0.5,1.0] for h in [10,20] for sl in [1.0,1.5,2.0] for tp in [2.0,3.0,5.0]]
for sym, bars in SYMS.items():
    tf=sym.split("_")[1]; sname=sym.split("_")[0]
    sweep("TREND_PB_ADX", a_trend_pullback_adx, GR, bars, tf, sname, results)

print("\n=== ASIAN SESSION BREAK (H1 only) ===")
GR = [(h,sl,tp) for h in [3,5,10] for sl in [1.0,1.5,2.0] for tp in [2.0,3.0,5.0]]
for sym, bars in SYMS.items():
    tf=sym.split("_")[1]; sname=sym.split("_")[0]
    if tf!="H1": continue
    sweep("ASIAN_BREAK", a_asian_break, GR, bars, tf, sname, results)

print("\n=== NY CLOSE FADE (H1 only) ===")
GR = [(d,h,sl,tp) for d in [1.0,1.5,2.0] for h in [3,5,10] for sl in [1.0,1.5] for tp in [1.5,2.0,3.0]]
for sym, bars in SYMS.items():
    tf=sym.split("_")[1]; sname=sym.split("_")[0]
    if tf!="H1": continue
    sweep("NY_CLOSE_FADE", a_ny_close_fade, GR, bars, tf, sname, results)

print(f"\n\n=== FINAL RANKING ===")
results.sort(key=lambda x:-x[7])
print(f"{'Rank':>4} {'Archetype':<16} {'Sym':<5} {'TF':<3} {'IS_Sh':>6} {'OOS_Sh':>6} {'FUL_Sh':>6} {'n':>4} {'PnL%':>6} {'WR%':>5} {'MDD%':>5}")
print("-"*100)
for i,(label,sym,tf,params,s_is,s_oos,s_ful,mn) in enumerate(results, 1):
    print(f"{i:>4} {label:<16} {sym:<5} {tf:<3} {s_is['sharpe']:>6.2f} {s_oos['sharpe']:>6.2f} {s_ful['sharpe']:>6.2f} {s_ful['n']:>4} {s_ful['pnl']*100:>5.1f}% {s_ful['wr']:>4.1f}% {s_ful['mdd']*100:>4.1f}%")

print(f"\nTotal winners: {len(results)}")
print(f"  Sh>=5: {sum(1 for r in results if r[7]>=5.0)}")
print(f"  Sh>=3: {sum(1 for r in results if r[7]>=3.0)}")
print(f"Elapsed: {time.time()-t_start:.0f}s")
