#!/usr/bin/env python3
"""
FX edge probe across multiple strategy archetypes that are NOT Donchian/MR
(which already failed on EURUSD). Tests:

  1. Overnight gap continuation (Sun-Mon open gap, Asian opening)
  2. Tokyo range -> London break (different from London-open which failed)
  3. NY-close H1 trend continuation (15:00 UTC trigger -> hold 4h)
  4. Weekly close trend (Mon-Fri direction follow)
  5. Volatility expansion entry (ATR > 2x avg -> trend follow)

Run per symbol. Reports cost-aware stats. Symbol candidates from merged files.
"""
import sys, os, csv, time, math
from collections import deque
from dataclasses import dataclass
from datetime import datetime, timezone

SYMBOLS = {
    "EURUSD":   dict(usd_per_pt=1000.0, pip=0.0001, spread=0.00010, name="EUR/USD"),
    "GBPUSD":   dict(usd_per_pt=1000.0, pip=0.0001, spread=0.00012, name="GBP/USD"),
    "USDJPY":   dict(usd_per_pt=10.0,   pip=0.01,   spread=0.012,   name="USD/JPY"),
}

@dataclass
class Bar: ts:int=0; o:float=0; h:float=0; l:float=0; c:float=0

def load_h1(path):
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

def stats(trades, sym):
    if not trades: return None
    cfg=SYMBOLS[sym]
    pnls=[t for t in trades]
    n=len(pnls); pnl=sum(pnls); wr=100*sum(1 for p in pnls if p>0)/n
    m=pnl/n; v=sum((p-m)**2 for p in pnls)/max(1,n-1); sd=math.sqrt(v)
    sh=(m/sd*math.sqrt(252)) if sd>0 else 0
    return dict(n=n,pnl=pnl,wr=wr,sharpe=sh)

# ── Strategy 1: weekend gap continuation ────────────────────────────────────
# At first H1 of Sun-Mon week, take direction of Fri-Sun gap; hold 12 bars.
def strat_weekend_gap(h1, sym, sl_pips=20, tp_pips=40, hold=12):
    cfg=SYMBOLS[sym]; trades=[]
    last_fri_close=None; last_day=-1
    active=False; entry=sl=tp=0; entry_idx=0; is_long=False
    for i,b in enumerate(h1):
        dt=datetime.fromtimestamp(b.ts,tz=timezone.utc)
        dow=dt.weekday(); hr=dt.hour
        if active:
            sl_hit=(b.l<=sl) if is_long else (b.h>=sl)
            tp_hit=(b.h>=tp) if is_long else (b.l<=tp)
            to=(i-entry_idx)>=hold
            if sl_hit:
                pts=(sl-entry) if is_long else (entry-sl)
                trades.append(pts*cfg["usd_per_pt"]*0.01 - cfg["spread"]*cfg["usd_per_pt"]*0.01); active=False
            elif tp_hit:
                pts=(tp-entry) if is_long else (entry-tp)
                trades.append(pts*cfg["usd_per_pt"]*0.01 - cfg["spread"]*cfg["usd_per_pt"]*0.01); active=False
            elif to:
                pts=(b.c-entry) if is_long else (entry-b.c)
                trades.append(pts*cfg["usd_per_pt"]*0.01 - cfg["spread"]*cfg["usd_per_pt"]*0.01); active=False
            continue
        if dow==4 and hr==20: last_fri_close=b.c
        # Mon 00 UTC: gap entry
        if dow==0 and hr==0 and last_fri_close is not None:
            gap=b.o-last_fri_close
            if abs(gap) >= 5*cfg["pip"]:
                is_long = gap>0
                entry=b.o
                sl = entry - sl_pips*cfg["pip"] if is_long else entry + sl_pips*cfg["pip"]
                tp = entry + tp_pips*cfg["pip"] if is_long else entry - tp_pips*cfg["pip"]
                active=True; entry_idx=i
    return trades

# ── Strategy 2: Tokyo range -> London expansion ─────────────────────────────
# Range = 22:00 prev to 06:00 UTC. Break at 07:00. Hold 6h.
def strat_tokyo_london(h1, sym, sl_atr=1.5, tp_atr=3.0, hold=8):
    cfg=SYMBOLS[sym]; atr=atr14(h1); trades=[]
    cur_day=-1; tk_hi=0; tk_lo=1e9; fired=False
    active=False; entry=sl=tp=0; entry_idx=0; is_long=False
    for i,b in enumerate(h1):
        dt=datetime.fromtimestamp(b.ts,tz=timezone.utc)
        dow=dt.weekday(); hr=dt.hour; day=b.ts//86400
        if day!=cur_day: cur_day=day; tk_hi=0; tk_lo=1e9; fired=False
        if active:
            sl_hit=(b.l<=sl) if is_long else (b.h>=sl)
            tp_hit=(b.h>=tp) if is_long else (b.l<=tp)
            to=(i-entry_idx)>=hold
            if sl_hit:
                pts=(sl-entry) if is_long else (entry-sl)
                trades.append(pts*cfg["usd_per_pt"]*0.01 - cfg["spread"]*cfg["usd_per_pt"]*0.01); active=False
            elif tp_hit:
                pts=(tp-entry) if is_long else (entry-tp)
                trades.append(pts*cfg["usd_per_pt"]*0.01 - cfg["spread"]*cfg["usd_per_pt"]*0.01); active=False
            elif to:
                pts=(b.c-entry) if is_long else (entry-b.c)
                trades.append(pts*cfg["usd_per_pt"]*0.01 - cfg["spread"]*cfg["usd_per_pt"]*0.01); active=False
            continue
        # Tokyo range 22-06 UTC = (prev day 22) to (this day 06)
        if hr>=22 or hr<6:
            if b.h>tk_hi: tk_hi=b.h
            if b.l<tk_lo: tk_lo=b.l
        # London open break trigger at hr==7
        if hr==7 and not fired and atr[i] is not None and tk_hi>0 and tk_lo<1e9 and dow<5:
            fired=True
            if b.c>tk_hi:
                is_long=True; entry=b.c
                sl=entry - sl_atr*atr[i]; tp=entry + tp_atr*atr[i]
                active=True; entry_idx=i
            elif b.c<tk_lo:
                is_long=False; entry=b.c
                sl=entry + sl_atr*atr[i]; tp=entry - tp_atr*atr[i]
                active=True; entry_idx=i
    return trades

# ── Strategy 3: NY-close H1 trend continuation ──────────────────────────────
# 20:00 UTC after NY: look at last 6h trend direction; enter at 21:00 close hold 6h
def strat_ny_trend(h1, sym, lookback=6, sl_atr=1.5, tp_atr=3.0, hold=6):
    cfg=SYMBOLS[sym]; atr=atr14(h1); trades=[]
    active=False; entry=sl=tp=0; entry_idx=0; is_long=False
    for i,b in enumerate(h1):
        dt=datetime.fromtimestamp(b.ts,tz=timezone.utc)
        hr=dt.hour; dow=dt.weekday()
        if active:
            sl_hit=(b.l<=sl) if is_long else (b.h>=sl)
            tp_hit=(b.h>=tp) if is_long else (b.l<=tp)
            to=(i-entry_idx)>=hold
            if sl_hit:
                pts=(sl-entry) if is_long else (entry-sl)
                trades.append(pts*cfg["usd_per_pt"]*0.01 - cfg["spread"]*cfg["usd_per_pt"]*0.01); active=False
            elif tp_hit:
                pts=(tp-entry) if is_long else (entry-tp)
                trades.append(pts*cfg["usd_per_pt"]*0.01 - cfg["spread"]*cfg["usd_per_pt"]*0.01); active=False
            elif to:
                pts=(b.c-entry) if is_long else (entry-b.c)
                trades.append(pts*cfg["usd_per_pt"]*0.01 - cfg["spread"]*cfg["usd_per_pt"]*0.01); active=False
            continue
        if hr==21 and dow<5 and i>=lookback and atr[i] is not None:
            change=h1[i].c - h1[i-lookback].c
            if abs(change) > 0.5*atr[i]:
                is_long=change>0
                entry=b.c
                sl=entry - sl_atr*atr[i] if is_long else entry + sl_atr*atr[i]
                tp=entry + tp_atr*atr[i] if is_long else entry - tp_atr*atr[i]
                active=True; entry_idx=i
    return trades

def main():
    candidates = [
        ("EURUSD","/Users/jo/Tick/EURUSD_merged.h1.csv"),
        ("GBPUSD","/Users/jo/Tick/GBPUSD_merged.h1.csv"),
        ("USDJPY","/Users/jo/Tick/USDJPY_merged.h1.csv"),
    ]
    results=[]
    for sym,path in candidates:
        if not os.path.exists(path):
            print(f"[{sym}] no h1 data at {path}, skip"); continue
        h1=load_h1(path)
        print(f"\n== {sym} h1 bars={len(h1)} ==")
        for sname, fn in [("WEEKEND_GAP",strat_weekend_gap),
                          ("TOKYO_LON", strat_tokyo_london),
                          ("NY_TREND",  strat_ny_trend)]:
            t=fn(h1, sym)
            s=stats(t, sym)
            if s is None or s["n"]<10:
                print(f"  [{sname}] n<10, skip"); continue
            tag=" ***" if s["sharpe"]>1.5 else ""
            print(f"  [{sname}] n={s['n']} PnL=${s['pnl']:.2f} WR={s['wr']:.1f}% Sh={s['sharpe']:.2f}{tag}")
            results.append((sym,sname,s))
    print("\n=== SUMMARY (Sharpe-sorted) ===")
    results.sort(key=lambda x:x[2]["sharpe"],reverse=True)
    for sym,sname,s in results:
        print(f"  {sym:<8} {sname:<14} n={s['n']:<4} PnL=${s['pnl']:>7.2f} WR={s['wr']:>4.1f}% Sh={s['sharpe']:>5.2f}")

if __name__ == "__main__":
    main()
