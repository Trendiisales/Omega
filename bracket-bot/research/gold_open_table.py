#!/usr/bin/env python3
"""Show actual Sunday-open weekly movements for 2y of gold.

Reveals why brackets at "average move" don't always win."""
import yfinance as yf
import pandas as pd
import numpy as np

gc = yf.download('GC=F', period='730d', interval='1h', progress=False, auto_adjust=False)
if isinstance(gc.columns, pd.MultiIndex):
    gc.columns = [c[0] for c in gc.columns]
gc = gc.reset_index()
gc.columns = [str(c).lower() for c in gc.columns]
ts_col = next((c for c in ['datetime','date','index','timestamp'] if c in gc.columns), gc.columns[0])
gc['ts'] = pd.to_datetime(gc[ts_col], utc=True)
gc = gc.sort_values('ts').reset_index(drop=True)
gc['gap_h'] = (gc['ts'].diff().dt.total_seconds()/3600).fillna(0)
opens = gc[gc['gap_h'] >= 12].copy().reset_index(drop=True)

rows = []
for _, op in opens.iterrows():
    open_ts = op['ts']
    open_px = op['open']
    end_ts = open_ts + pd.Timedelta(hours=120)  # 5d hold
    win = gc[(gc['ts'] >= open_ts) & (gc['ts'] <= end_ts)]
    if win.empty: continue
    # Identify which came first: max up or max down
    max_up_px  = win['high'].max()
    max_dn_px  = win['low'].min()
    max_up_idx = win['high'].idxmax()
    max_dn_idx = win['low'].idxmin()
    first_dir = 'UP' if max_up_idx < max_dn_idx else 'DN'
    max_up = max_up_px - open_px
    max_dn = open_px - max_dn_px
    fri_close = win.iloc[-1]['close']
    net = fri_close - open_px
    rows.append({
        'week': open_ts.strftime('%Y-%m-%d'),
        'open': open_px,
        'max_up': max_up,
        'max_dn': max_dn,
        'first': first_dir,
        'close': fri_close,
        'net': net,
    })

df = pd.DataFrame(rows)
print(f'{"date":<12} {"open":>8} {"max↑":>7} {"max↓":>7} {"first":>5} {"close":>8} {"net":>8}')
print('-'*60)
# Show ALL weeks
for _, r in df.tail(50).iterrows():
    arrow = '▲' if r['net'] > 0 else '▼'
    print(f'{r["week"]:<12} {r["open"]:>8.1f} {r["max_up"]:>+7.1f} {r["max_dn"]:>+7.1f} {r["first"]:>5} {r["close"]:>8.1f} {r["net"]:>+8.1f} {arrow}')

print()
print('=== AGGREGATE (last 2y, all weeks) ===')
print(f'Total weeks:                    {len(df)}')
print(f'Avg max excursion UP:           ${df["max_up"].mean():.1f}')
print(f'Avg max excursion DOWN:         ${df["max_dn"].mean():.1f}')
print(f'Avg NET (close - open):         ${df["net"].mean():+.1f}')
print(f'Median NET:                     ${df["net"].median():+.1f}')
print(f'% weeks NET positive:           {(df["net"]>0).sum()/len(df)*100:.0f}%')
print(f'% weeks NET negative:           {(df["net"]<0).sum()/len(df)*100:.0f}%')
print()
print('--- ASYMMETRY between MAX moves and NET ---')
print(f'Weeks max↑ > $50 BUT close NEGATIVE:  {((df["max_up"]>50) & (df["net"]<0)).sum()} of {(df["max_up"]>50).sum()}')
print(f'Weeks max↓ > $50 BUT close POSITIVE:  {((df["max_dn"]>50) & (df["net"]>0)).sum()} of {(df["max_dn"]>50).sum()}')
print(f'Weeks BOTH max↑>$30 AND max↓>$30:     {((df["max_up"]>30) & (df["max_dn"]>30)).sum()}  <- chop weeks')

print()
print('=== "AVERAGE BRACKET" SIM ===')
# Use avg as bracket offset
avg_up = df['max_up'].mean()
avg_dn = df['max_dn'].mean()
half = (avg_up + avg_dn) / 2
print(f'Avg UP move: ${avg_up:.0f}  Avg DN move: ${avg_dn:.0f}  Half-avg: ${half:.0f}')
print()
print('If we set bracket at AVG MOVE ($70 either side), what happens?')
off = 70
wins = 0; losses = 0; no_trig = 0; details = []
for _, r in df.iterrows():
    # Use the within-week excursion to determine if triggered + outcome
    # Trigger: max_up >= 70 (BUY triggers) OR max_dn >= 70 (SELL triggers)
    # Whichever FIRST -> entry
    if r['max_up'] >= off and r['max_dn'] >= off:
        # both happened. Use 'first'
        if r['first'] == 'UP':
            side='BUY'; entry=r['open']+off
        else:
            side='SELL'; entry=r['open']-off
    elif r['max_up'] >= off:
        side='BUY'; entry=r['open']+off
    elif r['max_dn'] >= off:
        side='SELL'; entry=r['open']-off
    else:
        no_trig += 1
        continue
    pnl = (r['close']-entry) if side=='BUY' else (entry-r['close'])
    if pnl > 0: wins += 1
    else: losses += 1
print(f'Trades: {wins+losses}  No trigger: {no_trig}  Wins: {wins}  Losses: {losses}  Win%: {wins/(wins+losses)*100:.0f}%')

print()
# Distribution
print('=== MAX_UP DISTRIBUTION ===')
for bins in [(0,20),(20,40),(40,60),(60,80),(80,120),(120,200),(200,400),(400,1000)]:
    lo,hi=bins
    n=((df['max_up']>=lo) & (df['max_up']<hi)).sum()
    bar='█'*int(n*40/len(df))
    print(f'  ${lo:>3}-${hi:>3}:  {n:>3} {bar}')

print()
print('=== MAX_DN DISTRIBUTION ===')
for bins in [(0,20),(20,40),(40,60),(60,80),(80,120),(120,200),(200,400),(400,1000)]:
    lo,hi=bins
    n=((df['max_dn']>=lo) & (df['max_dn']<hi)).sum()
    bar='█'*int(n*40/len(df))
    print(f'  ${lo:>3}-${hi:>3}:  {n:>3} {bar}')
