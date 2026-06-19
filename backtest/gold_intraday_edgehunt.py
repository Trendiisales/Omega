#!/usr/bin/env python3
"""
Gold intraday M15/M30 edge hunt — Donchian / Keltner / ATR-expansion breakout,
vol-target vs fixed sizing, no-TP ATR-trail vs fixed-R exit.
Cross-regime: 2022-2023 (bear/recovery) vs 2024-2026 (bull).
Cost: REAL IBKR gold ~$1.60/oz round-trip ($0.80/side).

Bars are pre-built OHLC. This is BAR-REPLAY (overstates ~0.5-0.7 PF) — survivors
flagged NEEDS FAITHFUL TICK BT.
"""
import csv, sys, math
from collections import defaultdict

COST_PER_SIDE = 0.80  # $/oz, round-trip $1.60

def load(path):
    rows = []
    with open(path) as f:
        r = csv.reader(f)
        hdr = next(r)
        # detect columns
        if hdr[0] in ('ts_unix','ts'):
            for row in r:
                rows.append((int(float(row[0])), float(row[1]), float(row[2]), float(row[3]), float(row[4])))
    return rows  # (ts,o,h,l,c)

def resample(bars, factor):
    """Aggregate `factor` consecutive bars into one."""
    out = []
    for i in range(0, len(bars) - factor + 1, factor):
        chunk = bars[i:i+factor]
        ts = chunk[0][0]
        o = chunk[0][1]
        h = max(b[2] for b in chunk)
        l = min(b[3] for b in chunk)
        c = chunk[-1][4]
        out.append((ts, o, h, l, c))
    return out

def atr(bars, n=14):
    """Wilder ATR series aligned to bars (atr[i] uses data up to i)."""
    tr = [0.0]*len(bars)
    for i in range(1, len(bars)):
        h,l,pc = bars[i][2], bars[i][3], bars[i-1][4]
        tr[i] = max(h-l, abs(h-pc), abs(l-pc))
    a = [None]*len(bars)
    if len(bars) <= n: return a
    seed = sum(tr[1:n+1])/n
    a[n] = seed
    for i in range(n+1, len(bars)):
        a[i] = (a[i-1]*(n-1) + tr[i])/n
    return a

def ema(vals, n):
    out = [None]*len(vals)
    k = 2/(n+1)
    e = None
    for i,v in enumerate(vals):
        e = v if e is None else (v - e)*k + e
        out[i] = e
    return out

def run(bars, signal, params, exit_mode, sizing, atr_n=14):
    """
    Returns list of trades: (entry_ts, side, entry_px, exit_px, qty)
    signal: 'donchian','keltner','atrexp'
    params: dict
    exit_mode: ('trail', mult) | ('fixedR', r_mult, stop_atr)
    sizing: 'fixed' | 'voltarget'
    One position at a time. Stop-and-reverse style for trend (flat then re-enter).
    """
    a = atr(bars, atr_n)
    closes = [b[4] for b in bars]
    ema20 = ema(closes, 20)
    trades = []
    pos = 0          # 0 flat, 1 long, -1 short
    entry_px = 0.0; qty = 0.0
    trail = None; stop = None; tp = None
    N = params.get('N', 20)
    target_risk = 100.0  # $ risk per trade for voltarget (notional scaler)

    start = max(N+2, atr_n+2)
    for i in range(start, len(bars)):
        ai = a[i]
        if ai is None or ai <= 0:
            continue
        o,h,l,c = bars[i][1], bars[i][2], bars[i][3], bars[i][4]
        prevc = bars[i-1][4]

        # ---- manage open position (check exit on this bar using H/L) ----
        if pos != 0:
            exit_now = False; ex_px = None
            if exit_mode[0] == 'trail':
                if pos == 1:
                    new_trail = h - exit_mode[1]*ai
                    trail = new_trail if trail is None else max(trail, new_trail)
                    if l <= trail:
                        ex_px = min(o, trail); exit_now = True
                else:
                    new_trail = l + exit_mode[1]*ai
                    trail = new_trail if trail is None else min(trail, new_trail)
                    if h >= trail:
                        ex_px = max(o, trail); exit_now = True
            elif exit_mode[0] == 'fixedR':
                rmult, stop_atr = exit_mode[1], exit_mode[2]
                if pos == 1:
                    if l <= stop:
                        ex_px = min(o, stop); exit_now = True
                    elif h >= tp:
                        ex_px = max(o, tp); exit_now = True
                else:
                    if h >= stop:
                        ex_px = max(o, stop); exit_now = True
                    elif l <= tp:
                        ex_px = min(o, tp); exit_now = True
            if exit_now:
                trades.append((bars[i][0], pos, entry_px, ex_px, qty))
                pos = 0; trail = None; stop = None; tp = None

        # ---- entry signals (only if flat) ----
        if pos == 0:
            long_sig = short_sig = False
            if signal == 'donchian':
                hh = max(bars[j][2] for j in range(i-N, i))
                ll = min(bars[j][3] for j in range(i-N, i))
                if h >= hh: long_sig = True
                if l <= ll: short_sig = True
                lpx = max(o, hh); spx = min(o, ll)
            elif signal == 'keltner':
                k = params['k']; e = ema20[i-1]
                if e is None: continue
                up = e + k*a[i-1]; dn = e - k*a[i-1]
                if h >= up: long_sig = True
                if l <= dn: short_sig = True
                lpx = max(o, up); spx = min(o, dn)
            elif signal == 'atrexp':
                x = params['x']
                rng = h - l
                if rng >= x*a[i-1]:
                    if c > o: long_sig = True; lpx = c
                    elif c < o: short_sig = True; spx = c
            # sizing
            if sizing == 'voltarget':
                qty = target_risk / (exit_mode[-1]*ai if exit_mode[0]=='fixedR' else 2.0*ai)
            else:
                qty = 1.0
            if long_sig:
                pos = 1; entry_px = lpx
                if exit_mode[0]=='fixedR':
                    stop = entry_px - exit_mode[2]*ai
                    tp = entry_px + exit_mode[1]*exit_mode[2]*ai
                trail = None
            elif short_sig:
                pos = -1; entry_px = spx
                if exit_mode[0]=='fixedR':
                    stop = entry_px + exit_mode[2]*ai
                    tp = entry_px - exit_mode[1]*exit_mode[2]*ai
                trail = None
    return trades

def stats(trades):
    if not trades:
        return dict(n=0, pf=0, net=0, gross_win=0, gross_loss=0, pnls=[])
    pnls = []
    for ts, side, ein, ex, qty in trades:
        gross = (ex - ein)*side*qty
        cost = COST_PER_SIDE*2*qty  # round trip, scaled by qty
        pnls.append(gross - cost)
    gw = sum(p for p in pnls if p>0)
    gl = -sum(p for p in pnls if p<0)
    pf = gw/gl if gl>0 else (999 if gw>0 else 0)
    return dict(n=len(pnls), pf=pf, net=sum(pnls), gross_win=gw, gross_loss=gl, pnls=pnls)

def fattail(trades):
    """PF after removing top-3 winning trades."""
    s = stats(trades)
    if s['n'] < 6: return None
    pnls = sorted(s['pnls'], reverse=True)
    rest = pnls[3:]
    gw = sum(p for p in rest if p>0)
    gl = -sum(p for p in rest if p<0)
    return gw/gl if gl>0 else (999 if gw>0 else 0)

# ---- build datasets ----
def datasets():
    d = {}
    # 2022-2023
    bear_m15 = load('/Users/jo/Tick/XAUUSD_2022_2023.m15.csv')
    bear_m30 = resample(bear_m15, 2)
    bear_h1  = load('/Users/jo/Tick/XAUUSD_2022_2023.h1.csv')
    # 2024-2026 (fresh)
    bull_m5  = load('/Users/jo/Tick/2yr_XAUUSD_tick_fresh.m5.csv')
    bull_m15 = resample(bull_m5, 3)
    bull_m30 = load('/Users/jo/Tick/2yr_XAUUSD_tick_fresh.m30.csv')
    bull_h1  = load('/Users/jo/Tick/2yr_XAUUSD_tick_fresh.h1.csv')
    d['M15'] = (bear_m15, bull_m15)
    d['M30'] = (bear_m30, bull_m30)
    d['H1']  = (bear_h1,  bull_h1)
    return d

if __name__ == '__main__':
    DS = datasets()
    for tf in ('M15','M30','H1'):
        be, bu = DS[tf]
        print(f"{tf}: bear={len(be)} bars, bull={len(bu)} bars")
