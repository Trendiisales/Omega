#!/usr/bin/env python3
"""Thorough test: bracket at 13:00 and 14:00 UTC daily on gold."""
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
gc['date'] = gc['ts'].dt.date
gc['dow']  = gc['ts'].dt.dayofweek  # 0=Mon, 6=Sun

# Daily realized vol + trend for regime tagging
daily = gc.groupby('date')['close'].last().to_frame('close')
daily['ret']  = daily['close'].pct_change()
daily['vol30'] = daily['ret'].rolling(30).std() * np.sqrt(252)
daily['ret30'] = daily['close'].pct_change(30)

print(f'Data: {len(gc)} rows, {gc["ts"].min()} -> {gc["ts"].max()}')

def sim(opens, off, tp, sl, hold_min):
    trades=[]
    for _, op in opens.iterrows():
        ots, opx = op['ts'], op['open']
        bt = opx+off; st = opx-off
        end = ots + pd.Timedelta(minutes=hold_min)
        win = gc[(gc['ts']>ots) & (gc['ts']<=end)]
        if win.empty: continue
        side=None; entry=None; eidx=None
        for idx,r in win.iterrows():
            if r['high']>=bt: side='BUY'; entry=bt; eidx=idx; break
            if r['low']<=st:  side='SELL'; entry=st; eidx=idx; break
        if side is None:
            trades.append({'pnl':0,'reason':'NO_TRIG','ts':ots})
            continue
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
        trades.append({'pnl':pnl,'side':side,'reason':'TP' if pnl==tp else ('SL' if pnl==-sl else 'TIME'),'ts':ots})
    return trades

def stats(trades):
    traded = [t for t in trades if t['reason']!='NO_TRIG']
    if not traded: return None
    wins = [t for t in traded if t['pnl']>0]
    losses = [t for t in traded if t['pnl']<=0]
    pos = sum(t['pnl'] for t in wins)
    neg = abs(sum(t['pnl'] for t in losses))
    pf = pos/neg if neg>0 else 99
    pnls = [t['pnl'] for t in traded]
    cum = np.cumsum(pnls)
    dd = (cum - np.maximum.accumulate(cum)).min()
    return {'n_open':len(trades),'n_trade':len(traded),'no_trig':len(trades)-len(traded),
            'win%':len(wins)/len(traded)*100,'sum':sum(pnls),'pf':pf,'dd':dd,
            'avg_w':pos/len(wins) if wins else 0,
            'avg_l':-neg/len(losses) if losses else 0}

# Filter to weekdays only (Mon-Fri), hour 13 or 14, and only FIRST bar of that hour each day
# Use BAR at exactly that hour
opens13 = gc[(gc['hour']==13) & (gc['dow']<5)].copy()
opens14 = gc[(gc['hour']==14) & (gc['dow']<5)].copy()
opens_both = pd.concat([opens13, opens14]).sort_values('ts').reset_index(drop=True)

print(f'\nOpens 13:00 UTC weekdays: {len(opens13)}')
print(f'Opens 14:00 UTC weekdays: {len(opens14)}')
print(f'Combined:                 {len(opens_both)}')

# === PARAM SWEEP ===
print('\n=== SWEEP (13:00 UTC bracket) ===')
print(f"{'off':>4} {'TP':>4} {'SL':>4} {'hold':>5} {'n':>4} {'win%':>5} {'sum$':>9} {'PF':>5} {'DD$':>7}")
best = []
for off in [2, 3, 5]:
    for tp in [20, 30, 50]:
        for sl in [2, 3, 5]:
            for hold in [60, 120]:
                t13 = sim(opens13, off, tp, sl, hold)
                s = stats(t13)
                if s is None or s['n_trade']<50: continue
                best.append(('13:00', off, tp, sl, hold, s))

# Top by Calmar
best.sort(key=lambda x: -x[5]['sum']/abs(x[5]['dd']) if x[5]['dd']<0 else -99)
print('\nTOP 10 by Calmar (13:00 UTC):')
for tag,off,tp,sl,hold,s in best[:10]:
    cal = s['sum']/abs(s['dd']) if s['dd']<0 else 99
    print(f"  off=${off} TP=${tp} SL=${sl} hold={hold}m  n={s['n_trade']} win={s['win%']:.0f}% sum=${s['sum']:+.1f} PF={s['pf']:.2f} DD=${s['dd']:+.1f} Calmar={cal:.1f}")

# 14:00 sweep
print('\nTOP 10 by Calmar (14:00 UTC):')
best14 = []
for off in [2, 3, 5]:
    for tp in [20, 30, 50]:
        for sl in [2, 3, 5]:
            for hold in [60, 120]:
                t14 = sim(opens14, off, tp, sl, hold)
                s = stats(t14)
                if s is None or s['n_trade']<50: continue
                best14.append(('14:00', off, tp, sl, hold, s))
best14.sort(key=lambda x: -x[5]['sum']/abs(x[5]['dd']) if x[5]['dd']<0 else -99)
for tag,off,tp,sl,hold,s in best14[:10]:
    cal = s['sum']/abs(s['dd']) if s['dd']<0 else 99
    print(f"  off=${off} TP=${tp} SL=${sl} hold={hold}m  n={s['n_trade']} win={s['win%']:.0f}% sum=${s['sum']:+.1f} PF={s['pf']:.2f} DD=${s['dd']:+.1f} Calmar={cal:.1f}")

# Combined 13+14
print('\nTOP 10 by Calmar (BOTH 13:00 + 14:00 UTC):')
best_both = []
for off in [2, 3, 5]:
    for tp in [20, 30, 50]:
        for sl in [2, 3, 5]:
            for hold in [60, 120]:
                t = sim(opens_both, off, tp, sl, hold)
                s = stats(t)
                if s is None or s['n_trade']<50: continue
                best_both.append(('BOTH', off, tp, sl, hold, s))
best_both.sort(key=lambda x: -x[5]['sum']/abs(x[5]['dd']) if x[5]['dd']<0 else -99)
for tag,off,tp,sl,hold,s in best_both[:10]:
    cal = s['sum']/abs(s['dd']) if s['dd']<0 else 99
    print(f"  off=${off} TP=${tp} SL=${sl} hold={hold}m  n={s['n_trade']} win={s['win%']:.0f}% sum=${s['sum']:+.1f} PF={s['pf']:.2f} DD=${s['dd']:+.1f} Calmar={cal:.1f}")

# === REGIME STABILITY on best combined config ===
print('\n=== YEAR-BY-YEAR STABILITY (best combined config) ===')
WINNER = (3, 50, 3, 60)  # default to validated; replace if sweep shows different
t = sim(opens_both, *WINNER)
opens_both['pnl'] = [tr['pnl'] for tr in t]
opens_both['year'] = opens_both['ts'].dt.year
for y in sorted(opens_both['year'].unique()):
    sub = opens_both[opens_both['year']==y]
    pnls = sub['pnl'].tolist()
    traded = [p for p in pnls if p != 0]
    if not traded: continue
    wins = sum(1 for p in traded if p>0)
    print(f"  {y}: n={len(traded):>3} win={wins/len(traded)*100:.0f}% sum=${sum(traded):+.1f}/oz")

# Bear vs bull split (by prior 30d return)
print('\n=== BEAR vs BULL split (best combined config) ===')
opens_both['ret30'] = opens_both['ts'].dt.date.map(lambda d: daily.loc[d, 'ret30'] if d in daily.index else None)
for label, cond in [('BEAR (prior30d < -2%)', opens_both['ret30'] < -0.02),
                    ('SIDEWAYS (-2% .. +2%)', (opens_both['ret30']>=-0.02) & (opens_both['ret30']<=0.02)),
                    ('BULL (prior30d > +2%)', opens_both['ret30'] > 0.02)]:
    sub = opens_both[cond]
    pnls = [p for p in sub['pnl'] if p != 0]
    if not pnls: continue
    wins = sum(1 for p in pnls if p>0)
    print(f"  {label:<28} n={len(pnls):>3} win={wins/len(pnls)*100:.0f}% sum=${sum(pnls):+.1f}/oz")
