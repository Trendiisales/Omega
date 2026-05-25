#!/usr/bin/env python3
"""Daily-open bracket sim — fires at EVERY CME session reopen.

CME globex: closes 22:00 UTC, reopens 23:00 UTC daily Mon-Thu.
Sunday: 46h gap (Fri 22:00 -> Sun 23:00).
Detect ALL reopen gaps and apply same bracket logic.
Compare Sunday-only vs every-daily-reopen.
"""
import yfinance as yf
import pandas as pd
import numpy as np

gc = yf.download('GC=F', period='730d', interval='1h', progress=False, auto_adjust=False)
if isinstance(gc.columns, pd.MultiIndex): gc.columns = [c[0] for c in gc.columns]
gc = gc.reset_index()
gc.columns = [str(c).lower() for c in gc.columns]
ts = next((c for c in ['datetime','date','index'] if c in gc.columns), gc.columns[0])
gc['ts'] = pd.to_datetime(gc[ts], utc=True)
gc = gc.sort_values('ts').reset_index(drop=True)
gc['gap_h'] = (gc['ts'].diff().dt.total_seconds()/3600).fillna(0)

# All gap-bars (reopens): any > 0.5h
all_reopens = gc[gc['gap_h'] >= 0.5].copy()
sunday_reopens = gc[gc['gap_h'] >= 12].copy()
daily_reopens = gc[(gc['gap_h'] >= 0.5) & (gc['gap_h'] < 12)].copy()

print(f'All reopens:     {len(all_reopens)}')
print(f'Sunday reopens:  {len(sunday_reopens)}  (gap >=12h)')
print(f'Daily reopens:   {len(daily_reopens)}  (0.5h <= gap < 12h)')
print()

# Gap distribution
print('=== GAP DISTRIBUTION (hours) ===')
for lo, hi in [(0.5, 1), (1, 1.5), (1.5, 2), (2, 3), (3, 6), (6, 12), (12, 24), (24, 48), (48, 72)]:
    n = ((all_reopens['gap_h'] >= lo) & (all_reopens['gap_h'] < hi)).sum()
    print(f'  {lo:>4.1f}h - {hi:>4.1f}h: {n:>4}')
print()

def sim_bracket(opens_df, off, tp, sl, hold_min, label):
    trades = []
    for _, op in opens_df.iterrows():
        ots, opx = op['ts'], op['open']
        bt = opx + off; st = opx - off
        end = ots + pd.Timedelta(minutes=hold_min)
        win = gc[(gc['ts'] > ots) & (gc['ts'] <= end)]
        if win.empty: continue
        side=None; entry=None; eidx=None
        for idx, r in win.iterrows():
            if r['high'] >= bt: side='BUY'; entry=bt; eidx=idx; break
            if r['low']  <= st: side='SELL'; entry=st; eidx=idx; break
        if side is None:
            trades.append({'pnl': 0, 'reason': 'NO_TRIG'})
            continue
        tp_px = entry+tp if side=='BUY' else entry-tp
        sl_px = entry-sl if side=='BUY' else entry+sl
        post = win.loc[eidx:]
        pnl = None
        for _, r in post.iterrows():
            if side == 'BUY':
                if r['low'] <= sl_px: pnl = -sl; break
                if r['high'] >= tp_px: pnl = tp; break
            else:
                if r['high'] >= sl_px: pnl = -sl; break
                if r['low'] <= tp_px: pnl = tp; break
        if pnl is None:
            cp = post.iloc[-1]['close']
            pnl = (cp-entry) if side=='BUY' else (entry-cp)
        trades.append({'pnl': pnl, 'side': side, 'reason': 'TP' if pnl==tp else ('SL' if pnl==-sl else 'TIME')})

    n = len(trades)
    traded = [t for t in trades if t['reason'] != 'NO_TRIG']
    wins = [t for t in traded if t['pnl'] > 0]
    losses = [t for t in traded if t['pnl'] <= 0]
    pos = sum(t['pnl'] for t in wins)
    neg = abs(sum(t['pnl'] for t in losses))
    pf = pos/neg if neg > 0 else 99
    cum = np.cumsum([t['pnl'] for t in traded])
    dd = (cum - np.maximum.accumulate(cum)).min() if len(cum) else 0
    total = sum(t['pnl'] for t in traded)
    return {
        'label': label, 'opens': n, 'traded': len(traded),
        'no_trig': n - len(traded),
        'win%': len(wins)/len(traded)*100 if traded else 0,
        'sum': total, 'pf': pf, 'dd': dd,
    }

# Compare configs
CONFIG = (3.0, 50.0, 3.0, 60)  # offset, TP, SL, hold_min (our winner)

print(f'=== BRACKET SIM (off=${CONFIG[0]}, TP=${CONFIG[1]}, SL=${CONFIG[2]}, hold={CONFIG[3]}min) ===')
for opens_df, label in [(sunday_reopens, 'Sunday-only'),
                        (daily_reopens, 'Daily (non-Sunday)'),
                        (all_reopens, 'EVERY reopen')]:
    r = sim_bracket(opens_df, *CONFIG, label)
    print(f"  {r['label']:<22} opens={r['opens']:>3} traded={r['traded']:>3} win%={r['win%']:>4.0f}% sum=${r['sum']:>+8.1f}/oz PF={r['pf']:>5.2f} DD=${r['dd']:>+6.1f}")

print()
# More configs on daily
print('=== DAILY-ONLY (non-Sunday) sweep ===')
print(f"{'off':>4} {'TP':>4} {'SL':>4} {'hold':>5} {'n':>4} {'win%':>5} {'sum$':>8} {'PF':>5} {'DD$':>7}")
for off in [2, 3, 5, 8]:
    for tp in [20, 30, 50]:
        for sl in [2, 3, 5]:
            for hold in [30, 60, 120]:
                r = sim_bracket(daily_reopens, off, tp, sl, hold, '')
                if r['traded'] < 50: continue
                print(f"${off:>3} ${tp:>3} ${sl:>3} {hold:>4}m {r['traded']:>4} {r['win%']:>4.0f}% {r['sum']:>+7.1f} {r['pf']:>5.2f} {r['dd']:>+6.1f}")

print()
# Time-of-day filter: when in UTC do daily reopens cluster?
daily_reopens['hour_utc'] = daily_reopens['ts'].dt.hour
print('=== DAILY REOPEN HOUR (UTC) ===')
hour_dist = daily_reopens['hour_utc'].value_counts().sort_index()
for h, c in hour_dist.items():
    bar = '█' * int(c * 40 / hour_dist.max())
    print(f'  {h:>2}:00 UTC  {c:>3}  {bar}')

# Run by-hour to find best window
print()
print('=== BEST DAILY HOUR (off=$3, TP=$50, SL=$3, 60m) ===')
for h in sorted(daily_reopens['hour_utc'].unique()):
    subset = daily_reopens[daily_reopens['hour_utc'] == h]
    if len(subset) < 20: continue
    r = sim_bracket(subset, *CONFIG, f'{h}:00 UTC')
    print(f"  {h:>2}:00 UTC n={r['traded']:>3} win%={r['win%']:>4.0f}% sum=${r['sum']:>+7.1f}/oz PF={r['pf']:>5.2f}")
