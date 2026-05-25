#!/usr/bin/env python3
"""Omniscient Paradox — adapted for gold-related universe with logic fixes.

Universe: GLD (gold) SLV (silver) PPLT (platinum) UUP (USD) TLT (bonds) BIL (cash)
Daily momentum rotation, single-holding, vol-targeted, with fixes:
  - Composite score uses ADD not MULTIPLY (avoids sign-inversion bug)
  - Rotation only when best > 0 AND beats current by margin
  - Vol target 20% (was 80%)
  - Hard SL @ -8% from entry
  - SMA50 trend filter as gate, not multiplier
  - RSI overbought penalty only (not oversold)
"""
import vectorbt as vbt
import pandas as pd
import numpy as np

UNIVERSE = ['GLD', 'SLV', 'PPLT', 'UUP', 'TLT', 'BIL']
SAFE = 'BIL'

print('Downloading 6y data...')
data = vbt.YFData.download(UNIVERSE, start='2019-01-01', missing_index='drop').get('Close')
print(f'shape: {data.shape}, range: {data.index[0]} -> {data.index[-1]}')

# Indicators per asset
roc9   = data.pct_change(9)
roc21  = data.pct_change(21)
roc63  = data.pct_change(63)
vol21  = data.pct_change().rolling(21).std() * np.sqrt(252)
sma50  = data.rolling(50).mean()
rsi    = vbt.RSI.run(data, window=14, ewm=False).rsi
# Flatten multi-level columns vbt creates
if hasattr(rsi.columns, 'levels'):
    rsi.columns = [c[-1] for c in rsi.columns]
sma200 = data.rolling(200).mean()

# Composite momentum score (FIX 1: weighted SUM not multiply)
weighted_mom = 0.5*roc9 + 0.3*roc21 + 0.2*roc63

# Risk-adjusted by vol (avoid div by 0)
risk_adj = weighted_mom / vol21.replace(0, np.nan)

# Trend GATE not multiplier (FIX 2): if below SMA50, score nullified to 0
above_sma = data > sma50
risk_adj_gated = risk_adj.where(above_sma, 0)

# RSI penalty (FIX 3): only overbought penalty (oversold = entry chance)
rsi_penalty = pd.DataFrame(1.0, index=rsi.index, columns=rsi.columns)
rsi_penalty[rsi > 80] = 0.7

score = risk_adj_gated * rsi_penalty

# Mask SAFE out (always 0 in scoring, used as parking)
score[SAFE] = 0

# === Selection: pick top each day ===
ROTATION_MARGIN = 0.10  # 10% improvement needed
SL_PCT = 0.08           # 8% hard stop loss
TARGET_VOL = 0.20        # 20% annual vol target (FIX 4)

n = len(data)
holding = [None] * n
entry_px = [None] * n

# Rank
top = score.idxmax(axis=1)
top_score = score.max(axis=1)

# Decision logic (sequential — can't fully vectorize w/ rotation memory)
cur_holding = SAFE
cur_entry = None
weights = pd.DataFrame(0.0, index=data.index, columns=data.columns)
sl_exits = 0
rot_changes = 0

for i in range(60, n):  # warmup 60 days
    px_today = data.iloc[i]
    best_asset = top.iloc[i]
    best_s = top_score.iloc[i]
    cur_s = score.iloc[i].get(cur_holding, -999) if cur_holding != SAFE else 0

    # Stop loss check
    if cur_holding != SAFE and cur_entry is not None:
        ret = px_today[cur_holding] / cur_entry - 1
        if ret <= -SL_PCT:
            cur_holding = SAFE
            cur_entry = None
            sl_exits += 1

    # Rotation decision (FIX 5: require best > 0 absolute)
    target = cur_holding
    if cur_holding == SAFE:
        if best_s > 0.05:  # threshold to leave cash
            target = best_asset
    else:
        # require POSITIVE best AND margin over current
        if best_s > 0 and best_s > cur_s * (1 + ROTATION_MARGIN) and best_asset != cur_holding:
            target = best_asset
        elif cur_s < 0:  # current went negative -> safe
            target = SAFE

    # Bear regime gate (FIX 6: replace UUP-pref with universal de-risk)
    spy_proxy_below = False  # could add SPY check but skipping for gold-focused

    if target != cur_holding:
        cur_holding = target
        cur_entry = px_today.get(target, None) if target != SAFE else None
        rot_changes += 1

    # Vol targeting weight
    if cur_holding != SAFE and cur_holding in vol21.columns:
        v = vol21.iloc[i].get(cur_holding, np.nan)
        if pd.notna(v) and v > 0:
            w = min(1.0, TARGET_VOL / v)
        else:
            w = 0.5
    else:
        w = 0.0

    # Assign
    weights.iloc[i] = 0
    if cur_holding == SAFE:
        weights.iloc[i, weights.columns.get_loc(SAFE)] = 1.0
    else:
        weights.iloc[i, weights.columns.get_loc(cur_holding)] = w
        rem = 1.0 - w
        if rem > 0.05:
            weights.iloc[i, weights.columns.get_loc(SAFE)] = rem

print(f'SL exits: {sl_exits}  Rotation changes: {rot_changes}')

# Backtest via vectorbt
pf = vbt.Portfolio.from_orders(
    close=data,
    size=weights,
    size_type='targetpercent',
    fees=0.0005,           # 5bp per trade
    slippage=0.0005,       # 5bp slip
    init_cash=100000,
    freq='1D',
    cash_sharing=True,
    group_by=True,
)

stats = pf.stats()
print()
print('=== BACKTEST STATS ===')
print(stats)

# Quick equity print
eq = pf.value()
print()
print(f'Start equity:  ${eq.iloc[0]:>12,.0f}')
print(f'End equity:    ${eq.iloc[-1]:>12,.0f}')
print(f'Total return:  {(eq.iloc[-1]/eq.iloc[0]-1)*100:>12.1f}%')
years = (eq.index[-1] - eq.index[0]).days/365.25
cagr = (eq.iloc[-1]/eq.iloc[0])**(1/years) - 1
print(f'Years:         {years:.1f}')
print(f'CAGR:          {cagr*100:>12.1f}%')
print(f'Max DD:        {(eq/eq.cummax()-1).min()*100:>12.1f}%')
