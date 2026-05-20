#!/usr/bin/env python3
"""
Second-pass edge probe. Tests:

  A. Long-only trend follow on H4 (EMA50>EMA200 -> ride pullbacks)
  B. Daily breakout entry (yest close + 0.5*ATR up = long, hold N days)
  C. H1 mean reversion (z-score fade on top of session)
  D. NSXUSD H1 mean-reversion (since H4 Donchian failed)
  E. Friday-close fade (close > Mon open by 1%+ -> Mon short)

Symbols: EURUSD, GBPUSD, USDJPY, NSXUSD, GER40, SPXUSD (SP).
"""
import sys, os, csv, time, math
from collections import deque
from dataclasses import dataclass
from datetime import datetime, timezone

SYMBOLS = {
    "EURUSD":   dict(usd_per_pt=1000.0, pip=0.0001, spread=0.00010,  atr_min=0.0008),
    "GBPUSD":   dict(usd_per_pt=1000.0, pip=0.0001, spread=0.00012,  atr_min=0.0008),
    "USDJPY":   dict(usd_per_pt=10.0,   pip=0.01,   spread=0.012,    atr_min=0.08),
    "NSXUSD":   dict(usd_per_pt=1.0,    pip=1.0,    spread=2.0,      atr_min=10.0),
    "GER40":    dict(usd_per_pt=25.0,   pip=1.0,    spread=2.0,      atr_min=8.0),
    "SPXUSD":   dict(usd_per_pt=50.0,   pip=0.25,   spread=0.50,     atr_min=2.0),
}

@dataclass
class Bar: ts:int=0; o:float=0; h:float=0; l:float=0; c:float=0

def load_bars(path):
    out=[]
    with open(path) as f:
        r=csv.reader(f); next(r,None)
        for row in r: out.append(Bar(int(row[0]),float(row[1]),float(row[2]),float(row[3]),float(row[4])))
    return out

def atr14(bars):
    out=[None]*len(bars); trs=deque(maxlen=14); pc=None
    for i,b in enumerate(bars):
        tr=b.h-b.l
        if pc is not None: tr=max(tr,abs(b.h-pc),abs(b.l-pc))
        trs.append(tr); pc=b.c
        if len(trs)==14: out[i]=sum(trs)/14
    return out

def ema(bars, period):
    out=[None]*len(bars); a=2/(period+1); v=None
    for i,b in enumerate(bars):
        if v is None: v=b.c
        else: v = a*b.c + (1-a)*v
        if i>=period: out[i]=v
    return out

def stats(trades, sym, cost_units=None):
    if not trades: return None
    cfg=SYMBOLS[sym]
    sp = cost_units if cost_units is not None else cfg["spread"]
    pnls=[t - sp*cfg["usd_per_pt"]*0.01 for t in trades]
    n=len(pnls); pnl=sum(pnls); wr=100*sum(1 for p in pnls if p>0)/n
    m=pnl/n; v=sum((p-m)**2 for p in pnls)/max(1,n-1); sd=math.sqrt(v)
    sh=(m/sd*math.sqrt(252)) if sd>0 else 0
    cum=0; pk=0; mdd=0
    for p in pnls:
        cum+=p
        if cum>pk: pk=cum
        if pk-cum>mdd: mdd=pk-cum
    return dict(n=n,pnl=pnl,wr=wr,sharpe=sh,mdd=mdd)

# ── A. Long-only H4 trend pullback (only enter long when EMA50>EMA200) ───────
def strat_long_pullback(h4, sym, ema_fast=20, ema_slow=50, pullback_atr=0.5,
                        sl_atr=1.5, tp_atr=4.0, hold=24):
    cfg=SYMBOLS[sym]; e_f=ema(h4,ema_fast); e_s=ema(h4,ema_slow); a=atr14(h4)
    trades=[]
    active=False; entry=sl=tp=0; entry_idx=0
    for i,b in enumerate(h4):
        if active:
            sl_hit=b.l<=sl; tp_hit=b.h>=tp; to=(i-entry_idx)>=hold
            if sl_hit:
                trades.append((sl-entry)*cfg["usd_per_pt"]*0.01); active=False
            elif tp_hit:
                trades.append((tp-entry)*cfg["usd_per_pt"]*0.01); active=False
            elif to:
                trades.append((b.c-entry)*cfg["usd_per_pt"]*0.01); active=False
            continue
        if e_f[i] is None or e_s[i] is None or a[i] is None: continue
        if a[i] < cfg["atr_min"]: continue
        if e_f[i] <= e_s[i]: continue  # only longs in uptrend
        # pullback condition: low touched within pullback_atr*ATR of e_f
        if b.l > e_f[i] + pullback_atr*a[i]: continue
        if b.c < e_f[i]: continue  # close back above
        entry=b.c
        sl=entry - sl_atr*a[i]; tp=entry + tp_atr*a[i]
        active=True; entry_idx=i
    return trades

# ── B. Daily breakout (close > yest close + 0.5*ATR = long, hold) ───────────
def strat_daily_breakout(daily, sym, mult=0.5, sl_atr=1.5, tp_atr=3.0, hold=5, long_only=True):
    cfg=SYMBOLS[sym]; a=atr14(daily); trades=[]
    active=False; entry=sl=tp=0; entry_idx=0; is_long=False
    for i,b in enumerate(daily):
        if active:
            sl_hit=(b.l<=sl) if is_long else (b.h>=sl)
            tp_hit=(b.h>=tp) if is_long else (b.l<=tp)
            to=(i-entry_idx)>=hold
            if sl_hit:
                pts=(sl-entry) if is_long else (entry-sl)
                trades.append(pts*cfg["usd_per_pt"]*0.01); active=False
            elif tp_hit:
                pts=(tp-entry) if is_long else (entry-tp)
                trades.append(pts*cfg["usd_per_pt"]*0.01); active=False
            elif to:
                pts=(b.c-entry) if is_long else (entry-b.c)
                trades.append(pts*cfg["usd_per_pt"]*0.01); active=False
            continue
        if i<14 or a[i] is None: continue
        prev=daily[i-1].c
        up=prev + mult*a[i]; dn=prev - mult*a[i]
        if b.c > up:
            is_long=True; entry=b.c
            sl=entry - sl_atr*a[i]; tp=entry + tp_atr*a[i]
            active=True; entry_idx=i
        elif not long_only and b.c < dn:
            is_long=False; entry=b.c
            sl=entry + sl_atr*a[i]; tp=entry - tp_atr*a[i]
            active=True; entry_idx=i
    return trades

# ── C. H1 z-score mean reversion (intraday session-bound) ───────────────────
def strat_h1_mr(h1, sym, win=20, z_in=2.0, sl_atr=1.0, tp_atr=1.0, hold=8):
    cfg=SYMBOLS[sym]; a=atr14(h1); trades=[]
    active=False; entry=sl=tp=0; entry_idx=0; is_long=False
    rolls=deque(maxlen=win)
    for i,b in enumerate(h1):
        rolls.append(b.c)
        if active:
            sl_hit=(b.l<=sl) if is_long else (b.h>=sl)
            tp_hit=(b.h>=tp) if is_long else (b.l<=tp)
            to=(i-entry_idx)>=hold
            if sl_hit:
                pts=(sl-entry) if is_long else (entry-sl)
                trades.append(pts*cfg["usd_per_pt"]*0.01); active=False
            elif tp_hit:
                pts=(tp-entry) if is_long else (entry-tp)
                trades.append(pts*cfg["usd_per_pt"]*0.01); active=False
            elif to:
                pts=(b.c-entry) if is_long else (entry-b.c)
                trades.append(pts*cfg["usd_per_pt"]*0.01); active=False
            continue
        if len(rolls)<win or a[i] is None: continue
        m=sum(rolls)/win; sd=math.sqrt(sum((x-m)**2 for x in rolls)/(win-1))
        if sd<=0: continue
        z=(b.c-m)/sd
        if z>z_in:  # over-extended up -> fade short
            is_long=False; entry=b.c
            sl=entry + sl_atr*a[i]; tp=entry - tp_atr*a[i]
            active=True; entry_idx=i
        elif z<-z_in:  # over-extended down -> fade long
            is_long=True; entry=b.c
            sl=entry - sl_atr*a[i]; tp=entry + tp_atr*a[i]
            active=True; entry_idx=i
    return trades

def main():
    datasets=[
        # h1 only and h4-derive
        ("EURUSD","/Users/jo/Tick/EURUSD_merged.h1.csv","/Users/jo/Tick/EURUSD_merged.h4.csv"),
        ("GBPUSD","/Users/jo/Tick/GBPUSD_merged.h1.csv","/Users/jo/Tick/GBPUSD_merged.h4.csv"),
        ("USDJPY","/Users/jo/Tick/USDJPY_merged.h1.csv","/Users/jo/Tick/USDJPY_merged.h4.csv"),
        ("NSXUSD","/Users/jo/Tick/NSXUSD_merged.h1.csv","/Users/jo/Tick/NSXUSD_merged.h4.csv"),
        ("GER40", "/Users/jo/Tick/GER40_merged.h1.csv", "/Users/jo/Tick/GER40_merged.h4.csv"),
        ("SPXUSD","/Users/jo/Tick/SPXUSD_merged.h1.csv","/Users/jo/Tick/SPXUSD_merged.h4.csv"),
    ]
    results=[]
    for sym,h1p,h4p in datasets:
        if not os.path.exists(h1p) or not os.path.exists(h4p):
            print(f"[{sym}] no data, skip"); continue
        h1=load_bars(h1p); h4=load_bars(h4p)
        # daily aggregate from h4 (6 h4 bars per day approx)
        daily=[]; cur=None; cur_day=-1
        for b in h4:
            d=b.ts//86400
            if d!=cur_day:
                if cur: daily.append(cur)
                cur_day=d; cur=Bar(b.ts,b.o,b.h,b.l,b.c)
            else:
                cur.h=max(cur.h,b.h); cur.l=min(cur.l,b.l); cur.c=b.c
        if cur: daily.append(cur)
        print(f"\n== {sym} h1={len(h1)} h4={len(h4)} d1={len(daily)} ==")
        for sname, fn, src in [("LONG_PB_H4", strat_long_pullback, h4),
                               ("D1_BREAK",  strat_daily_breakout, daily),
                               ("H1_MR_Z2",  lambda b,s: strat_h1_mr(b,s,z_in=2.0), h1),
                               ("H1_MR_Z25", lambda b,s: strat_h1_mr(b,s,z_in=2.5), h1)]:
            t=fn(src, sym)
            s=stats(t, sym)
            if s is None or s["n"]<10:
                print(f"  [{sname}] n<10, skip"); continue
            tag=" ***" if s["sharpe"]>1.5 else (" *" if s["sharpe"]>0.5 else "")
            print(f"  [{sname:<11}] n={s['n']:<4} PnL=${s['pnl']:>7.2f} WR={s['wr']:>4.1f}% Sh={s['sharpe']:>5.2f} MDD=${s['mdd']:>5.2f}{tag}")
            results.append((sym,sname,s))
    print("\n=== TOP-15 by Sharpe ===")
    results.sort(key=lambda x:x[2]["sharpe"],reverse=True)
    for sym,sname,s in results[:15]:
        print(f"  {sym:<8} {sname:<11} n={s['n']:<4} PnL=${s['pnl']:>7.2f} WR={s['wr']:>4.1f}% Sh={s['sharpe']:>5.2f}")

if __name__ == "__main__":
    main()
