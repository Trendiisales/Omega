#!/usr/bin/env python3
"""London-open windows sweep on gold."""
import yfinance as yf
import pandas as pd
import numpy as np

gc = yf.download('GC=F', period='730d', interval='1h', progress=False, auto_adjust=False)
if isinstance(gc.columns, pd.MultiIndex): gc.columns = [c[0] for c in gc.columns]
gc = gc.reset_index()
gc.columns = [str(c).lower() for c in gc.columns]
tsc = next((c for c in ['datetime','date','index'] if c in gc.columns), gc.columns[0])
gc['ts'] = pd.to_datetime(gc[tsc], utc=True)
gc = gc.sort_values('ts').reset_index(drop=True)
gc['hour'] = gc['ts'].dt.hour
gc['dow'] = gc['ts'].dt.dayofweek

def sim(opens, off, tp, sl, hold_min):
    trades=[]
    for _, op in opens.iterrows():
        ots, opx = op['ts'], op['open']
        bt=opx+off; st=opx-off
        end = ots + pd.Timedelta(minutes=hold_min)
        win = gc[(gc['ts']>ots) & (gc['ts']<=end)]
        if win.empty: continue
        side=None; entry=None; eidx=None
        for idx,r in win.iterrows():
            if r['high']>=bt: side='BUY'; entry=bt; eidx=idx; break
            if r['low']<=st:  side='SELL'; entry=st; eidx=idx; break
        if side is None: trades.append({'pnl':0,'reason':'NO_TRIG'}); continue
        tp_px = entry+tp if side=='BUY' else entry-tp
        sl_px = entry-sl if side=='BUY' else entry+sl
        post = win.loc[eidx:]
        pnl=None
        for _,r in post.iterrows():
            if side=='BUY':
                if r['low']<=sl_px: pnl=-sl; break
                if r['high']>=tp_px: pnl=tp; break
            else:
                if r['high']>=sl_px: pnl=-sl; break
                if r['low']<=tp_px: pnl=tp; break
        if pnl is None:
            cp = post.iloc[-1]['close']
            pnl = (cp-entry) if side=='BUY' else (entry-cp)
        trades.append({'pnl':pnl,'reason':'TP' if pnl==tp else ('SL' if pnl==-sl else 'TIME')})
    return trades

def stats(trades):
    traded = [t for t in trades if t['reason']!='NO_TRIG']
    if not traded: return None
    wins = [t for t in traded if t['pnl']>0]
    losses = [t for t in traded if t['pnl']<=0]
    pos = sum(t['pnl'] for t in wins); neg = abs(sum(t['pnl'] for t in losses))
    pf = pos/neg if neg>0 else 99
    pnls = [t['pnl'] for t in traded]
    cum = np.cumsum(pnls); dd = (cum - np.maximum.accumulate(cum)).min()
    return {'n':len(traded),'win%':len(wins)/len(traded)*100,'sum':sum(pnls),'pf':pf,'dd':dd}

print('=== LONDON HOURS SWEEP (off=$2, TP=$50, SL=$2, hold=60m) ===')
print(f"{'hour':>5} {'n':>4} {'win%':>5} {'sum$':>8} {'PF':>5} {'DD$':>7} {'Calmar':>7}")
for h in [6, 7, 8, 9, 10, 11]:
    opens = gc[(gc['hour']==h) & (gc['dow']<5)]
    t = sim(opens, 2, 50, 2, 60)
    s = stats(t)
    if s is None: continue
    cal = s['sum']/abs(s['dd']) if s['dd']<0 else 99
    print(f"  {h:>2}:00 {s['n']:>4} {s['win%']:>4.0f}% {s['sum']:>+7.1f} {s['pf']:>5.2f} {s['dd']:>+6.1f} {cal:>6.1f}")

print()
print('=== TOP COMBOS for 07:00 + 08:00 UTC (best 2-window London) ===')
opens78 = gc[(gc['hour'].isin([7,8])) & (gc['dow']<5)]
best=[]
for off in [2,3,5]:
    for tp in [20,30,50]:
        for sl in [2,3]:
            for hold in [60, 120]:
                t = sim(opens78, off, tp, sl, hold)
                s = stats(t)
                if s is None or s['n']<30: continue
                best.append((off,tp,sl,hold,s))
best.sort(key=lambda x: -(x[4]['sum']/abs(x[4]['dd']) if x[4]['dd']<0 else 99))
print(f"{'off':>4} {'TP':>4} {'SL':>4} {'hold':>5} {'n':>4} {'win%':>5} {'sum$':>8} {'PF':>5} {'DD$':>7} {'Calmar':>7}")
for off,tp,sl,hold,s in best[:10]:
    cal = s['sum']/abs(s['dd']) if s['dd']<0 else 99
    print(f"${off:>3} ${tp:>3} ${sl:>3} {hold:>4}m {s['n']:>4} {s['win%']:>4.0f}% {s['sum']:>+7.1f} {s['pf']:>5.2f} {s['dd']:>+6.1f} {cal:>6.1f}")

print()
print('=== FULL 4-window combo: 07+08+13+14 UTC ===')
opens_all = gc[(gc['hour'].isin([7,8,13,14])) & (gc['dow']<5)]
print(f'Combined opens: {len(opens_all)}')
best=[]
for off in [2,3]:
    for tp in [30,50]:
        for sl in [2,3]:
            for hold in [60, 120]:
                t = sim(opens_all, off, tp, sl, hold)
                s = stats(t)
                if s is None: continue
                best.append((off,tp,sl,hold,s))
best.sort(key=lambda x: -(x[4]['sum']/abs(x[4]['dd']) if x[4]['dd']<0 else 99))
for off,tp,sl,hold,s in best[:10]:
    cal = s['sum']/abs(s['dd']) if s['dd']<0 else 99
    print(f"  off=${off} TP=${tp} SL=${sl} hold={hold}m n={s['n']} win={s['win%']:.0f}% sum=${s['sum']:+.1f} PF={s['pf']:.2f} DD=${s['dd']:+.1f} Calmar={cal:.1f}")
