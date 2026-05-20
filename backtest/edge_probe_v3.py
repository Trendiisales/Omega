#!/usr/bin/env python3
"""
Third-pass probes. Tests less-standard strategies:

  P. EURUSD/GBPUSD cointegration pairs (spread z-score fade)
  V. ATR contraction breakout (NR4-style: ATR(5)<0.7*ATR(20) -> daily break)
  M. Momentum (close > N-bar high AND ema_fast > ema_slow -> long, ATR trail)
  C. Carry-trend USDJPY (always long when EMA200 rising)
"""
import os, csv, math
from collections import deque
from dataclasses import dataclass

SYMBOLS = {
    "EURUSD":   dict(usd_per_pt=1000.0, pip=0.0001, spread=0.00010),
    "GBPUSD":   dict(usd_per_pt=1000.0, pip=0.0001, spread=0.00012),
    "USDJPY":   dict(usd_per_pt=10.0,   pip=0.01,   spread=0.012),
    "NSXUSD":   dict(usd_per_pt=1.0,    pip=1.0,    spread=2.0),
    "GER40":    dict(usd_per_pt=25.0,   pip=1.0,    spread=2.0),
}

@dataclass
class Bar: ts:int=0; o:float=0; h:float=0; l:float=0; c:float=0

def load(path):
    out=[]
    with open(path) as f:
        r=csv.reader(f); next(r,None)
        for row in r: out.append(Bar(int(row[0]),float(row[1]),float(row[2]),float(row[3]),float(row[4])))
    return out

def atrN(bars, n=14):
    out=[None]*len(bars); trs=deque(maxlen=n); pc=None
    for i,b in enumerate(bars):
        tr=b.h-b.l
        if pc is not None: tr=max(tr,abs(b.h-pc),abs(b.l-pc))
        trs.append(tr); pc=b.c
        if len(trs)==n: out[i]=sum(trs)/n
    return out

def ema(bars, p):
    out=[None]*len(bars); a=2/(p+1); v=None
    for i,b in enumerate(bars):
        v = b.c if v is None else a*b.c+(1-a)*v
        if i>=p: out[i]=v
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

# ── Pairs: cointegrate EURUSD vs GBPUSD ───────────────────────────────────
def strat_pairs(eur_h1, gbp_h1, win=120, z_in=2.0, z_out=0.5, hold=24):
    # Align bars by ts (intersection)
    eur_idx={b.ts:b for b in eur_h1}
    common=[(b.ts, b, eur_idx[b.ts]) for b in gbp_h1 if b.ts in eur_idx]
    print(f"  [pairs] common ts={len(common)}")
    if len(common)<200: return []
    # Spread = EUR - hedge*GBP where hedge from rolling regression
    spreads=[]; ts_list=[]
    for ts, gbp, eur in common:
        spreads.append(eur.c - gbp.c)  # simple spread, since both ~ USD pair
        ts_list.append(ts)
    trades=[]
    active=False; entry_spread=0; is_long=False; entry_idx=0
    pnls=[]
    for i in range(len(spreads)):
        if i<win: continue
        win_data=spreads[i-win:i]
        m=sum(win_data)/win
        sd=math.sqrt(sum((x-m)**2 for x in win_data)/(win-1))
        if sd<=0: continue
        z=(spreads[i]-m)/sd
        if active:
            # exit when z crosses back to z_out
            cur_z=(spreads[i]-m)/sd
            hit_out = (cur_z <= z_out) if is_long else (cur_z >= -z_out)
            stopped = (cur_z < -3.5) if is_long else (cur_z > 3.5)
            timed = (i-entry_idx)>=hold
            if hit_out or stopped or timed:
                pnl_spread = (spreads[i]-entry_spread) if is_long else (entry_spread-spreads[i])
                # $value: assume both legs 0.01 lot, each pair has different $/pip
                # EUR-GBP spread move: combined ~ 50% EUR drag + 50% GBP drag at $1000/unit
                pnls.append(pnl_spread * 500 - 0.0001 * 2000)  # 2 cost legs
                active=False
            continue
        if z > z_in:
            is_long=False; entry_spread=spreads[i]; active=True; entry_idx=i
        elif z < -z_in:
            is_long=True; entry_spread=spreads[i]; active=True; entry_idx=i
    return pnls

# ── Vol contraction breakout ────────────────────────────────────────────────
def strat_vol_break(daily, sym, atr_short=5, atr_long=20, contract=0.7,
                    sl_atr=1.5, tp_atr=4.0, hold=5, long_only=True):
    cfg=SYMBOLS[sym]; a_s=atrN(daily,atr_short); a_l=atrN(daily,atr_long)
    pnls=[]
    active=False; entry=sl=tp=0; entry_idx=0; is_long=False
    for i,b in enumerate(daily):
        if active:
            sl_hit=(b.l<=sl) if is_long else (b.h>=sl)
            tp_hit=(b.h>=tp) if is_long else (b.l<=tp)
            to=(i-entry_idx)>=hold
            if sl_hit:
                pts=(sl-entry) if is_long else (entry-sl)
                pnls.append(pts*cfg["usd_per_pt"]*0.01 - cfg["spread"]*cfg["usd_per_pt"]*0.01); active=False
            elif tp_hit:
                pts=(tp-entry) if is_long else (entry-tp)
                pnls.append(pts*cfg["usd_per_pt"]*0.01 - cfg["spread"]*cfg["usd_per_pt"]*0.01); active=False
            elif to:
                pts=(b.c-entry) if is_long else (entry-b.c)
                pnls.append(pts*cfg["usd_per_pt"]*0.01 - cfg["spread"]*cfg["usd_per_pt"]*0.01); active=False
            continue
        if i<atr_long or a_s[i] is None or a_l[i] is None: continue
        if a_s[i] >= contract * a_l[i]: continue  # need contraction
        prev=daily[i-1].c
        if b.c > prev + 0.5*a_l[i]:
            is_long=True; entry=b.c
            sl=entry - sl_atr*a_l[i]; tp=entry + tp_atr*a_l[i]
            active=True; entry_idx=i
        elif (not long_only) and b.c < prev - 0.5*a_l[i]:
            is_long=False; entry=b.c
            sl=entry + sl_atr*a_l[i]; tp=entry - tp_atr*a_l[i]
            active=True; entry_idx=i
    return pnls

# ── Trend-momentum with EMA regime + N-bar high break (H4) ─────────────────
def strat_trend_mom(h4, sym, n_high=20, ema_fast=50, ema_slow=200,
                    sl_atr=2.0, tp_atr=5.0, hold=24):
    cfg=SYMBOLS[sym]; a=atrN(h4); ef=ema(h4,ema_fast); es=ema(h4,ema_slow)
    pnls=[]
    active=False; entry=sl=tp=0; entry_idx=0
    for i,b in enumerate(h4):
        if active:
            sl_hit=b.l<=sl; tp_hit=b.h>=tp; to=(i-entry_idx)>=hold
            if sl_hit:
                pnls.append((sl-entry)*cfg["usd_per_pt"]*0.01 - cfg["spread"]*cfg["usd_per_pt"]*0.01); active=False
            elif tp_hit:
                pnls.append((tp-entry)*cfg["usd_per_pt"]*0.01 - cfg["spread"]*cfg["usd_per_pt"]*0.01); active=False
            elif to:
                pnls.append((b.c-entry)*cfg["usd_per_pt"]*0.01 - cfg["spread"]*cfg["usd_per_pt"]*0.01); active=False
            continue
        if i<=ema_slow or ef[i] is None or es[i] is None or a[i] is None: continue
        if ef[i] <= es[i]: continue  # only longs in uptrend
        if i<n_high: continue
        prior_high=max(bb.h for bb in h4[i-n_high:i])
        if b.c > prior_high:
            entry=b.c; sl=entry - sl_atr*a[i]; tp=entry + tp_atr*a[i]
            active=True; entry_idx=i
    return pnls

def main():
    print("== EUR-GBP pairs (H1, full data) ==")
    eur=load("/Users/jo/Tick/EURUSD_merged.h1.csv")
    gbp=load("/Users/jo/Tick/GBPUSD_merged.h1.csv")
    for zin in [1.5, 2.0, 2.5]:
        for w in [60, 120, 240]:
            t=strat_pairs(eur, gbp, win=w, z_in=zin)
            s=stats(t)
            if s and s["n"]>=10:
                tag=" ***" if s["sharpe"]>1.5 else (" *" if s["sharpe"]>0.5 else "")
                print(f"  win={w} z_in={zin}: n={s['n']:<3} PnL=${s['pnl']:>6.2f} WR={s['wr']:>4.1f}% Sh={s['sharpe']:>5.2f} MDD=${s['mdd']:>5.2f}{tag}")

    print("\n== Vol-contraction daily breakout ==")
    for sym, h4p in [("EURUSD","/Users/jo/Tick/EURUSD_merged.h4.csv"),
                     ("GBPUSD","/Users/jo/Tick/GBPUSD_merged.h4.csv"),
                     ("USDJPY","/Users/jo/Tick/USDJPY_merged.h4.csv"),
                     ("NSXUSD","/Users/jo/Tick/NSXUSD_merged.h4.csv"),
                     ("GER40", "/Users/jo/Tick/GER40_merged.h4.csv")]:
        if not os.path.exists(h4p): continue
        h4=load(h4p)
        # daily aggregate
        daily=[]; cur=None; cd=-1
        for b in h4:
            d=b.ts//86400
            if d!=cd:
                if cur: daily.append(cur)
                cd=d; cur=Bar(b.ts,b.o,b.h,b.l,b.c)
            else: cur.h=max(cur.h,b.h); cur.l=min(cur.l,b.l); cur.c=b.c
        if cur: daily.append(cur)
        for c in [0.6,0.7,0.8]:
            t=strat_vol_break(daily, sym, contract=c)
            s=stats(t)
            if s and s["n"]>=10:
                tag=" ***" if s["sharpe"]>1.5 else (" *" if s["sharpe"]>0.5 else "")
                print(f"  {sym:<8} contract={c}: n={s['n']:<3} PnL=${s['pnl']:>9.2f} WR={s['wr']:>4.1f}% Sh={s['sharpe']:>5.2f}{tag}")

    print("\n== H4 trend-momentum N-bar breakout (EMA50>EMA200 regime) ==")
    for sym, h4p in [("EURUSD","/Users/jo/Tick/EURUSD_merged.h4.csv"),
                     ("GBPUSD","/Users/jo/Tick/GBPUSD_merged.h4.csv"),
                     ("USDJPY","/Users/jo/Tick/USDJPY_merged.h4.csv"),
                     ("NSXUSD","/Users/jo/Tick/NSXUSD_merged.h4.csv"),
                     ("GER40", "/Users/jo/Tick/GER40_merged.h4.csv")]:
        if not os.path.exists(h4p): continue
        h4=load(h4p)
        for nh in [10, 20, 40]:
            t=strat_trend_mom(h4, sym, n_high=nh)
            s=stats(t)
            if s and s["n"]>=10:
                tag=" ***" if s["sharpe"]>1.5 else (" *" if s["sharpe"]>0.5 else "")
                print(f"  {sym:<8} n_high={nh}: n={s['n']:<3} PnL=${s['pnl']:>9.2f} WR={s['wr']:>4.1f}% Sh={s['sharpe']:>5.2f}{tag}")

if __name__=="__main__":
    main()
