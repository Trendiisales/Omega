#!/usr/bin/env python3
"""Final optimization sweep — pick config that maximizes return per DD."""
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
opens = gc[gc['gap_h'] >= 12].copy().reset_index(drop=True)

def sim(off, tp, hold_min, sl_mult=None):
    """sl_mult: if set, SL at offset*sl_mult from entry (None = use other bracket as SL)."""
    trades = []
    for _, op in opens.iterrows():
        ots, opx = op['ts'], op['open']
        bt, st = opx+off, opx-off
        end = ots + pd.Timedelta(minutes=hold_min)
        win = gc[(gc['ts']>ots) & (gc['ts']<=end)]
        if win.empty: continue
        side=None; entry=None; eidx=None
        for idx,r in win.iterrows():
            if r['high']>=bt: side='BUY'; entry=bt; eidx=idx; break
            if r['low']<=st:  side='SELL'; entry=st; eidx=idx; break
        if side is None: continue
        tp_px = entry+tp if side=='BUY' else entry-tp
        sl_px = entry - (off*sl_mult) if (sl_mult and side=='BUY') else (entry + (off*sl_mult) if sl_mult else None)
        post = win.loc[eidx:]
        pnl=None
        for _,r in post.iterrows():
            if sl_px is not None:
                if side=='BUY' and r['low']<=sl_px: pnl = -off*sl_mult; break
                if side=='SELL' and r['high']>=sl_px: pnl = -off*sl_mult; break
            if side=='BUY' and r['high']>=tp_px: pnl=tp; break
            if side=='SELL' and r['low']<=tp_px: pnl=tp; break
        if pnl is None:
            cp = post.iloc[-1]['close']
            pnl = (cp-entry) if side=='BUY' else (entry-cp)
        trades.append(pnl)
    if not trades: return None
    arr = np.array(trades)
    wins = (arr>0).sum()
    pos = arr[arr>0].sum()
    neg = abs(arr[arr<=0].sum())
    pf = pos/neg if neg>0 else 99
    cum = arr.cumsum()
    dd = (cum - np.maximum.accumulate(cum)).min()
    total = arr.sum()
    sharpe = arr.mean() / arr.std() * np.sqrt(50) if arr.std()>0 else 0  # 50 trades/yr proxy
    calmar = total / abs(dd) if dd<0 else 99
    return {'n':len(arr),'win%':wins/len(arr)*100,'sum':total,'pf':pf,'dd':dd,'sharpe':sharpe,'calmar':calmar}

print('Sweep: offset × TP × hold × SL_variant')
print(f"{'off':>4} {'TP':>4} {'hold':>5} {'SL':>4} {'n':>4} {'win%':>5} {'sum':>8} {'PF':>5} {'maxDD':>7} {'Sharpe':>6} {'Calmar':>6}")
print('-'*78)
results = []
for off in [3, 5, 8, 10, 15]:
    for tp in [10, 15, 20, 25, 30, 50]:
        for hold in [60, 90, 120, 180]:
            for sl_mult in [None, 1.0, 1.5, 2.0]:
                r = sim(off, tp, hold, sl_mult)
                if r is None or r['n']<30: continue
                sl_str = f'{sl_mult}x' if sl_mult else 'none'
                results.append((off,tp,hold,sl_mult,r))

# Sort by Calmar (return / max DD) - best risk-adjusted
print('=== TOP 10 by CALMAR (return/maxDD) ===')
results.sort(key=lambda x: -x[4]['calmar'])
for off,tp,hold,sl,r in results[:10]:
    sl_str = f'{sl}x' if sl else 'none'
    print(f"${off:>3} ${tp:>3} {hold:>4}m {sl_str:>4} {r['n']:>4} {r['win%']:>4.0f}% {r['sum']:>+7.1f} {r['pf']:>5.2f} {r['dd']:>+6.1f} {r['sharpe']:>5.2f} {r['calmar']:>5.2f}")

print()
print('=== TOP 10 by TOTAL $ ===')
results.sort(key=lambda x: -x[4]['sum'])
for off,tp,hold,sl,r in results[:10]:
    sl_str = f'{sl}x' if sl else 'none'
    print(f"${off:>3} ${tp:>3} {hold:>4}m {sl_str:>4} {r['n']:>4} {r['win%']:>4.0f}% {r['sum']:>+7.1f} {r['pf']:>5.2f} {r['dd']:>+6.1f} {r['sharpe']:>5.2f} {r['calmar']:>5.2f}")

print()
print('=== TOP 10 by PF ===')
results.sort(key=lambda x: -x[4]['pf'])
for off,tp,hold,sl,r in results[:10]:
    sl_str = f'{sl}x' if sl else 'none'
    print(f"${off:>3} ${tp:>3} {hold:>4}m {sl_str:>4} {r['n']:>4} {r['win%']:>4.0f}% {r['sum']:>+7.1f} {r['pf']:>5.2f} {r['dd']:>+6.1f} {r['sharpe']:>5.2f} {r['calmar']:>5.2f}")
