#!/usr/bin/env python3
"""Omniscient Paradox adapted to 2 tech indices.

Test sets:
 A) Leveraged: TQQQ (3x QQQ) + SOXL (3x semis) + BIL
 B) Unlevered: QQQ + SOXX + BIL

With FIXED logic (no multiplicative trend score, real bear gate, min hold).
"""
import vectorbt as vbt
import pandas as pd
import numpy as np

def run_rotation(universe, safe, start='2019-01-01',
                 min_hold=10, rot_margin=0.20, safe_threshold=0.05,
                 target_vol=0.35, fees=0.0002, slip=0.0002):
    data = vbt.YFData.download(universe + [safe, 'SPY'], start=start, missing_index='drop').get('Close')
    syms = universe + [safe]
    prices = data[syms]
    spy = data['SPY']
    spy_sma200 = spy.rolling(200).mean()
    spy_ok = spy > spy_sma200

    # Indicators for each universe asset
    roc9 = prices.pct_change(9)
    roc21 = prices.pct_change(21)
    roc63 = prices.pct_change(63)
    vol21 = prices.pct_change().rolling(21).std() * np.sqrt(252)
    sma50 = prices.rolling(50).mean()

    weighted = 0.5*roc9 + 0.3*roc21 + 0.2*roc63
    risk_adj = weighted / vol21.replace(0, np.nan)
    above_50 = prices > sma50
    score = risk_adj.where(above_50, -np.inf)
    score[safe] = 0  # safe = neutral, never picked as best

    n = len(prices)
    w = pd.DataFrame(0.0, index=prices.index, columns=syms)
    cur = safe
    entry_i = -999
    rot_count = 0
    sl_count = 0
    sl_pct = 0.10  # 10% hard stop (leveraged needs wider)
    cur_entry_px = None

    for i in range(80, n):
        # Hard SL check
        if cur != safe and cur_entry_px is not None:
            ret = prices.iloc[i][cur] / cur_entry_px - 1
            if ret <= -sl_pct:
                cur = safe
                cur_entry_px = None
                entry_i = i
                sl_count += 1

        # Regime gate
        if not spy_ok.iloc[i]:
            target = safe
        else:
            s = score.iloc[i].drop(safe)
            best = s.idxmax()
            best_s = s.max()
            cur_s = score.iloc[i].get(cur, -999) if cur != safe else 0

            if cur == safe:
                target = best if best_s > safe_threshold else safe
            else:
                # require POSITIVE best AND margin AND past min-hold
                if (best_s > 0 and best != cur
                    and best_s > cur_s * (1 + rot_margin)
                    and i - entry_i >= min_hold):
                    target = best
                elif cur_s < -0.02:
                    target = safe
                else:
                    target = cur

        if target != cur:
            cur = target
            entry_i = i
            cur_entry_px = prices.iloc[i][cur] if cur != safe else None
            rot_count += 1

        # Vol-targeted weight
        if cur != safe:
            v = vol21.iloc[i][cur]
            if pd.notna(v) and v > 0:
                weight = min(1.0, target_vol / v)
            else:
                weight = 0.5
        else:
            weight = 1.0

        w.iloc[i] = 0
        w.iloc[i, syms.index(cur)] = weight
        if cur != safe and weight < 1.0:
            w.iloc[i, syms.index(safe)] = 1.0 - weight

    pf = vbt.Portfolio.from_orders(close=prices, size=w, size_type='targetpercent',
                                   fees=fees, slippage=slip, init_cash=100000, freq='1D',
                                   cash_sharing=True, group_by=True)
    return pf, data, rot_count, sl_count

def report(name, pf, baseline_price=None):
    eq = pf.value()
    years = (eq.index[-1] - eq.index[0]).days / 365.25
    tot = (eq.iloc[-1]/eq.iloc[0]-1)*100
    cagr = (eq.iloc[-1]/eq.iloc[0])**(1/years) - 1
    dd = (eq/eq.cummax()-1).min()*100
    rets = eq.pct_change().dropna()
    sharpe = rets.mean()/rets.std() * np.sqrt(252) if rets.std() > 0 else 0
    bh_str = ''
    if baseline_price is not None:
        bh_tot = (baseline_price.iloc[-1]/baseline_price.iloc[0]-1)*100
        bh_str = f'  BH {bh_tot:>+7.1f}%'
    print(f'{name:<40} total {tot:>+7.1f}%  CAGR {cagr*100:>+6.1f}%  DD {dd:>+6.1f}%  Sharpe {sharpe:>5.2f}{bh_str}')

print('=== SET A: LEVERAGED TECH (TQQQ + SOXL) ===')
pf_a, data_a, rot, sl = run_rotation(['TQQQ','SOXL'], 'BIL')
report('Rotation TQQQ+SOXL+BIL', pf_a)
print(f'  rotations: {rot}  SL exits: {sl}')

pf = vbt.Portfolio.from_holding(data_a['TQQQ'], init_cash=100000, freq='1D')
report('Buy-Hold TQQQ', pf)
pf = vbt.Portfolio.from_holding(data_a['SOXL'], init_cash=100000, freq='1D')
report('Buy-Hold SOXL', pf)
# 50/50
half = (data_a['TQQQ'] / data_a['TQQQ'].iloc[0] + data_a['SOXL'] / data_a['SOXL'].iloc[0]) / 2
print(f'Buy-Hold 50/50 TQQQ+SOXL              total {(half.iloc[-1]/half.iloc[0]-1)*100:>+7.1f}%  CAGR {((half.iloc[-1]/half.iloc[0])**(1/((half.index[-1]-half.index[0]).days/365.25))-1)*100:>+6.1f}%  DD {(half/half.cummax()-1).min()*100:>+6.1f}%')

print()
print('=== SET B: UNLEVERAGED TECH (QQQ + SOXX) ===')
pf_b, data_b, rot_b, sl_b = run_rotation(['QQQ','SOXX'], 'BIL', target_vol=0.20)
report('Rotation QQQ+SOXX+BIL', pf_b)
print(f'  rotations: {rot_b}  SL exits: {sl_b}')

pf = vbt.Portfolio.from_holding(data_b['QQQ'], init_cash=100000, freq='1D')
report('Buy-Hold QQQ', pf)
pf = vbt.Portfolio.from_holding(data_b['SOXX'], init_cash=100000, freq='1D')
report('Buy-Hold SOXX', pf)

print()
print('=== SET C: TIGHTER ROTATION on LEVERAGED (margin 30%, min_hold 20d) ===')
pf_c, _, rot_c, sl_c = run_rotation(['TQQQ','SOXL'], 'BIL', min_hold=20, rot_margin=0.30, safe_threshold=0.10, target_vol=0.40)
report('Rotation TQQQ+SOXL strict', pf_c)
print(f'  rotations: {rot_c}  SL exits: {sl_c}')
