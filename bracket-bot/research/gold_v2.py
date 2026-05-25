#!/usr/bin/env python3
"""Iteration 2: simpler gold strategy variants.

V2a: GLD trend follow (above SMA200 = long, below = cash)
V2b: GLD + cash, momentum-only entry, tighter rotation
V2c: Gold metals rotation with strict regime filter + min-hold

Compare each to buy-and-hold GLD.
"""
import vectorbt as vbt
import pandas as pd
import numpy as np

data = vbt.YFData.download(['GLD','SLV','PPLT','UUP','TLT','BIL','SPY'],
                           start='2019-01-01', missing_index='drop').get('Close')
gld = data['GLD']
print(f'data shape: {data.shape}, range {data.index[0].date()} -> {data.index[-1].date()}')

# Buy-hold baselines
buyhold_gld = (gld.iloc[-1]/gld.iloc[0] - 1) * 100
buyhold_spy = (data['SPY'].iloc[-1]/data['SPY'].iloc[0] - 1) * 100
print(f'Buy-Hold GLD: {buyhold_gld:+.1f}%')
print(f'Buy-Hold SPY: {buyhold_spy:+.1f}%')

years = (data.index[-1] - data.index[0]).days / 365.25

def report(name, eq):
    tot = (eq.iloc[-1]/eq.iloc[0]-1)*100
    cagr = (eq.iloc[-1]/eq.iloc[0])**(1/years) - 1
    dd = (eq/eq.cummax()-1).min()*100
    rets = eq.pct_change().dropna()
    sharpe = rets.mean()/rets.std() * np.sqrt(252) if rets.std() > 0 else 0
    print(f'{name:<35} total {tot:>+7.1f}%  CAGR {cagr*100:>+6.1f}%  DD {dd:>+6.1f}%  Sharpe {sharpe:>5.2f}')

# === V2a: GLD trend follow ===
sma200 = gld.rolling(200).mean()
sma50 = gld.rolling(50).mean()
v2a_long = gld > sma200
pf = vbt.Portfolio.from_signals(
    close=gld,
    entries=v2a_long & ~v2a_long.shift(1).fillna(False),
    exits=~v2a_long & v2a_long.shift(1).fillna(False),
    fees=0.0001, slippage=0.0002, init_cash=100000, freq='1D')
report('V2a GLD>SMA200 trend', pf.value())

# === V2b: GLD momentum (composite > threshold) with min-hold ===
roc21 = gld.pct_change(21)
roc63 = gld.pct_change(63)
mom = 0.5*roc21 + 0.5*roc63
above_sma = gld > sma50
long_signal = (mom > 0.02) & above_sma
# enforce 10-day min hold to cut churn
last_entry_idx = -999
state = pd.Series(False, index=gld.index)
holding = False
entry_i = -999
for i in range(len(gld)):
    if holding:
        # exit if mom turns negative OR price below SMA50
        if mom.iloc[i] < -0.01 or not above_sma.iloc[i]:
            if i - entry_i >= 10:
                holding = False
        state.iloc[i] = holding
    else:
        if long_signal.iloc[i] and (i - entry_i) >= 10:
            holding = True
            entry_i = i
            state.iloc[i] = True

entries = state & ~state.shift(1).fillna(False)
exits = ~state & state.shift(1).fillna(False)
pf = vbt.Portfolio.from_signals(close=gld, entries=entries, exits=exits,
                                fees=0.0001, slippage=0.0002, init_cash=100000, freq='1D')
report('V2b GLD mom + SMA50 + 10d hold', pf.value())

# === V2c: gold metals rotation, regime-gated, min-hold ===
UNI = ['GLD','SLV','PPLT']
prices = data[UNI]
mom_uni = 0.5*prices.pct_change(21) + 0.5*prices.pct_change(63)
sma50_uni = prices.rolling(50).mean()
above50 = prices > sma50_uni
score = mom_uni.where(above50, -np.inf)
# regime filter: SPY > SMA200 to allow any trade
spy_sma200 = data['SPY'].rolling(200).mean()
spy_ok = data['SPY'] > spy_sma200

n = len(prices)
w = pd.DataFrame(0.0, index=prices.index, columns=['GLD','SLV','PPLT','BIL'])
cur = 'BIL'; entry_i = -999; MIN_HOLD = 15
for i in range(80, n):
    if not spy_ok.iloc[i]:
        target = 'BIL'
    else:
        s = score.iloc[i]
        if (s > 0).any() and i - entry_i >= MIN_HOLD:
            best = s.idxmax()
            if cur != best:
                target = best
            else:
                target = cur
        else:
            # current still OK?
            if cur in score.columns and score.iloc[i][cur] > 0:
                target = cur
            else:
                target = 'BIL'
    if target != cur:
        cur = target; entry_i = i
    w.iloc[i, w.columns.get_loc(cur)] = 1.0
# vbt portfolio from_orders
full = data[['GLD','SLV','PPLT','BIL']]
pf = vbt.Portfolio.from_orders(
    close=full, size=w, size_type='targetpercent',
    fees=0.0001, slippage=0.0002, init_cash=100000, freq='1D',
    cash_sharing=True, group_by=True)
report('V2c metals rotation min-15d', pf.value())

# === V2d: Just GLD with no fancy rules, but only when SPY trend up ===
v2d = (gld > sma200) & spy_ok
v2d_e = v2d & ~v2d.shift(1).fillna(False)
v2d_x = ~v2d & v2d.shift(1).fillna(False)
pf = vbt.Portfolio.from_signals(close=gld, entries=v2d_e, exits=v2d_x,
                                fees=0.0001, slippage=0.0002, init_cash=100000, freq='1D')
report('V2d GLD>SMA200 + SPY regime ok', pf.value())

# Baseline
pf = vbt.Portfolio.from_holding(gld, init_cash=100000, freq='1D')
report('-- buy-hold GLD baseline --', pf.value())
