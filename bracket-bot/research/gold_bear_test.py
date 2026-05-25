#!/usr/bin/env python3
"""Stress-test Sunday bracket in bear regimes.

Splits 2y data into:
  - Year-by-year stability
  - Bull weeks (prior 30d up) vs Bear weeks (prior 30d down)
  - Volatility regimes (high vol vs low vol)
"""
import yfinance as yf
import pandas as pd
import numpy as np

gc = yf.download('GC=F', period='730d', interval='1h', progress=False, auto_adjust=False)
if isinstance(gc.columns, pd.MultiIndex):
    gc.columns = [c[0] for c in gc.columns]
gc = gc.reset_index()
gc.columns = [str(c).lower() for c in gc.columns]
ts_col = next((c for c in ['datetime','date','index'] if c in gc.columns), gc.columns[0])
gc['ts'] = pd.to_datetime(gc[ts_col], utc=True)
gc = gc.sort_values('ts').reset_index(drop=True)
gc['gap_h'] = (gc['ts'].diff().dt.total_seconds()/3600).fillna(0)
opens = gc[gc['gap_h'] >= 12].copy().reset_index(drop=True)
print(f'Sunday opens: {len(opens)}')

# Daily aggregate for trend / vol metrics
daily = gc.copy()
daily['date'] = daily['ts'].dt.date
daily_close = daily.groupby('date').last().reset_index()
daily_close['ret'] = daily_close['close'].pct_change()
daily_close['ret30'] = daily_close['close'].pct_change(30)
daily_close['vol30'] = daily_close['ret'].rolling(30).std() * np.sqrt(252)

def sim_trade(open_ts, open_px, off=5, tp=20, hold_min=60):
    end = open_ts + pd.Timedelta(minutes=hold_min)
    win = gc[(gc['ts']>open_ts) & (gc['ts']<=end)]
    if win.empty: return None
    bt = open_px+off; st = open_px-off
    side=None; entry=None; eidx=None
    for idx,r in win.iterrows():
        if r['high']>=bt: side='BUY'; entry=bt; eidx=idx; break
        if r['low']<=st:  side='SELL'; entry=st; eidx=idx; break
    if side is None: return {'pnl':0, 'side':None, 'reason':'NO_TRIG'}
    tp_px = entry+tp if side=='BUY' else entry-tp
    post = win.loc[eidx:]
    pnl=None
    for _,r in post.iterrows():
        if side=='BUY' and r['high']>=tp_px: pnl=tp; break
        if side=='SELL' and r['low']<=tp_px: pnl=tp; break
    if pnl is None:
        cp = post.iloc[-1]['close']
        pnl = (cp-entry) if side=='BUY' else (entry-cp)
    return {'pnl':pnl, 'side':side, 'reason':'TP' if pnl==tp else 'TIME'}

def annotate(open_ts):
    """Get prior 30d return and vol for this Sunday."""
    d = open_ts.date()
    # find latest daily row before this Sunday
    prior = daily_close[daily_close['date'] < d]
    if len(prior) < 30: return None, None
    return prior.iloc[-1]['ret30'], prior.iloc[-1]['vol30']

# Run on each Sunday with annotations
rows = []
for _, op in opens.iterrows():
    r = sim_trade(op['ts'], op['open'])
    if r is None: continue
    ret30, vol30 = annotate(op['ts'])
    rows.append({
        'date': op['ts'],
        'year': op['ts'].year,
        'open': op['open'],
        'pnl': r['pnl'],
        'side': r['side'],
        'reason': r['reason'],
        'prior_30d_ret': ret30,
        'prior_30d_vol': vol30,
    })

df = pd.DataFrame(rows)
print()

def summary(label, sub):
    if len(sub) == 0: return
    traded = sub[sub['reason'] != 'NO_TRIG']
    wins = (traded['pnl'] > 0).sum()
    pos = traded[traded['pnl']>0]['pnl'].sum()
    neg = abs(traded[traded['pnl']<=0]['pnl'].sum())
    pf = pos/neg if neg>0 else 99
    print(f'  {label:<35} weeks={len(sub):>3} trades={len(traded):>3} win={wins}/{len(traded)} ({wins/max(len(traded),1)*100:.0f}%)  sum=${traded["pnl"].sum():+.1f}/oz  PF={pf:.2f}')

print('=== YEAR-BY-YEAR ===')
for y in sorted(df['year'].unique()):
    summary(f'Year {y}', df[df['year']==y])

print()
print('=== BEAR vs BULL (by prior 30d gold ret) ===')
summary('Bear weeks (prior 30d < -2%)', df[df['prior_30d_ret'] < -0.02])
summary('Sideways (-2% <= prior 30d <= +2%)', df[(df['prior_30d_ret']>=-0.02) & (df['prior_30d_ret']<=0.02)])
summary('Bull weeks (prior 30d > +2%)', df[df['prior_30d_ret'] > 0.02])

print()
print('=== VOL REGIMES (by prior 30d realized vol) ===')
vol_lo = df['prior_30d_vol'].quantile(0.33)
vol_hi = df['prior_30d_vol'].quantile(0.66)
summary(f'Low vol (< {vol_lo:.2f})',     df[df['prior_30d_vol'] < vol_lo])
summary(f'Mid vol',                       df[(df['prior_30d_vol']>=vol_lo) & (df['prior_30d_vol']<vol_hi)])
summary(f'High vol (>= {vol_hi:.2f})',    df[df['prior_30d_vol'] >= vol_hi])

print()
print('=== SIDE BREAKDOWN (BUY vs SELL bracket trigger) ===')
summary('BUY triggered',  df[df['side']=='BUY'])
summary('SELL triggered', df[df['side']=='SELL'])

print()
print('=== WORST DRAWDOWN PERIODS ===')
# Show worst 10 trades
worst = df.nsmallest(10, 'pnl')[['date','side','reason','prior_30d_ret','pnl']]
print(worst.to_string(index=False))

print()
print('=== EQUITY CURVE BY MONTH ===')
df['ym'] = df['date'].dt.to_period('M')
by_month = df.groupby('ym')['pnl'].agg(['count','sum','mean'])
print(by_month.to_string())
print()
cum = df['pnl'].cumsum()
print(f'Final cum: ${cum.iloc[-1]:+.1f}/oz')
print(f'Max DD in cum: ${(cum - cum.cummax()).min():+.1f}/oz')
