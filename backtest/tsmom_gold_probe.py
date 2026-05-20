#!/usr/bin/env python3
"""Probe TSMom-style daily momentum on XAUUSD with current data.

Signal: log-return momentum over N days, enter when ratio > threshold,
exit on opposite signal or hold timeout. Vol-targeted sizing.
"""
import csv, math
from datetime import datetime
from collections import deque

# Daily XAU bars: YYYYMMDD,O,H,L,C
bars=[]
with open("/Users/jo/Tick/2yr_XAUUSD_daily.csv") as f:
    for line in f:
        p=line.strip().split(",")
        if len(p)<5: continue
        try:
            d=p[0]
            dt=datetime(int(d[:4]), int(d[4:6]), int(d[6:8]))
            bars.append((int(dt.timestamp()), float(p[1]), float(p[2]), float(p[3]), float(p[4])))
        except: continue
print(f"Daily bars: {len(bars)}")

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

def simulate_tsmom(bars, lookback, hold_max, sl_atr, tp_atr, cost_pct=0.0001, long_only=True, ts_lo=0, ts_hi=10**12):
    """Time-series momentum: enter long if past N-day return > 0 (long_only)
    or enter direction of past N-day return otherwise. Exit at TP/SL/timeout."""
    pnls=[]
    n=len(bars)
    if n<lookback+15: return pnls
    # ATR14 over daily
    atrs=[None]*n; trs=deque(maxlen=14); pc=None
    for i,(_,_,h,l,c) in enumerate(bars):
        tr=h-l
        if pc is not None: tr=max(tr, abs(h-pc), abs(l-pc))
        trs.append(tr); pc=c
        if len(trs)==14: atrs[i]=sum(trs)/14
    active=False; entry=sl=tp=0; entry_idx=0; is_long=False
    for i in range(lookback, n):
        ts = bars[i][0]
        if ts<ts_lo or ts>=ts_hi: continue
        if active:
            h=bars[i][2]; l=bars[i][3]; c=bars[i][4]
            sl_hit=(l<=sl) if is_long else (h>=sl)
            tp_hit=(h>=tp) if is_long else (l<=tp)
            timed=(i-entry_idx)>=hold_max
            if sl_hit:
                r=(sl-entry)/entry if is_long else (entry-sl)/entry
                pnls.append(r - cost_pct); active=False
            elif tp_hit:
                r=(tp-entry)/entry if is_long else (entry-tp)/entry
                pnls.append(r - cost_pct); active=False
            elif timed:
                r=(c-entry)/entry if is_long else (entry-c)/entry
                pnls.append(r - cost_pct); active=False
            continue
        if atrs[i] is None: continue
        # Momentum: ratio of last lookback close to current close
        past=bars[i-lookback][4]; cur=bars[i][4]
        ret = math.log(cur/past)
        if abs(ret) < 0.005: continue  # need at least 0.5% momentum over lookback
        if long_only and ret < 0: continue
        is_long = ret > 0
        entry = cur
        atr_pct = atrs[i] / cur
        sl = entry * (1 - sl_atr*atr_pct) if is_long else entry * (1 + sl_atr*atr_pct)
        tp = entry * (1 + tp_atr*atr_pct) if is_long else entry * (1 - tp_atr*atr_pct)
        active=True; entry_idx=i
    return pnls

# Sweep
print("\n=== Daily momentum sweep (long_only=True, cost=10bps) ===")
best=None
for lb in [3, 5, 10, 15, 20, 30, 60]:
    for hold in [5, 10, 20, 40]:
        for sl in [1.0, 1.5, 2.0, 3.0]:
            for tp in [2.0, 3.0, 5.0, 8.0]:
                p = simulate_tsmom(bars, lb, hold, sl, tp, cost_pct=0.0010, long_only=True)
                s = stats(p)
                if not s or s["n"]<10: continue
                if s["sharpe"]>1.0:
                    print(f"  lb={lb:<2} h={hold:<2} sl={sl} tp={tp}: n={s['n']:<3} PnL={s['pnl']*100:>6.2f}% Sh={s['sharpe']:>5.2f}")
                if best is None or s["sharpe"]>best[1]["sharpe"]:
                    best = ((lb,hold,sl,tp), s)
if best:
    (lb,hold,sl,tp), s = best
    print(f"\nBEST(full): lb={lb} hold={hold} sl={sl} tp={tp}: n={s['n']} PnL={s['pnl']*100:.2f}% Sh={s['sharpe']:.2f}")

# Re-sweep with IS/OOS criterion: max min(IS_sh, OOS_sh) with n>=30 full
print("\n=== ROBUST IS/OOS sweep (require n>=30 full + both halves positive) ===")
mid_ts = bars[len(bars)//2][0]
results = []
for lb in [3, 5, 10, 15, 20, 30, 60]:
    for hold in [5, 10, 20, 40]:
        for sl in [1.0, 1.5, 2.0, 3.0]:
            for tp in [2.0, 3.0, 5.0, 8.0]:
                p_is  = simulate_tsmom(bars, lb, hold, sl, tp, cost_pct=0.0010, long_only=True, ts_lo=bars[0][0], ts_hi=mid_ts)
                p_oos = simulate_tsmom(bars, lb, hold, sl, tp, cost_pct=0.0010, long_only=True, ts_lo=mid_ts, ts_hi=bars[-1][0]+1)
                p_ful = simulate_tsmom(bars, lb, hold, sl, tp, cost_pct=0.0010, long_only=True)
                s_is = stats(p_is); s_oos = stats(p_oos); s_ful = stats(p_ful)
                if not s_is or not s_oos or not s_ful: continue
                if s_ful["n"]<30: continue
                if s_is["sharpe"]<=0 or s_oos["sharpe"]<=0: continue
                results.append((lb,hold,sl,tp, s_is, s_oos, s_ful, min(s_is["sharpe"], s_oos["sharpe"])))
results.sort(key=lambda x:-x[7])
print(f"{'cfg':<30} {'IS_n':>4} {'IS_Sh':>6} {'OOS_n':>5} {'OOS_Sh':>6} {'FUL_n':>5} {'FUL_Sh':>6} {'min':>5}")
print("-"*90)
for lb,hold,sl,tp, s_is,s_oos,s_ful, mn in results[:15]:
    cfg = f"lb={lb} h={hold} sl={sl} tp={tp}"
    print(f"{cfg:<30} {s_is['n']:>4} {s_is['sharpe']:>6.2f} {s_oos['n']:>5} {s_oos['sharpe']:>6.2f} {s_ful['n']:>5} {s_ful['sharpe']:>6.2f} {mn:>5.2f}")

