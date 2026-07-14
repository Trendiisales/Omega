#!/usr/bin/env python3
"""LondonFixMomentum DST-timing backtest.

Replicates live engine (GoldEngineStack.hpp LondonFixMomentumEngine, as-observed
in ledger): prefix bar = mid open->close over 30min window before the fix; fire
first bar after fix time if |move|>=1.0pt; LONG on move>0 else SHORT.
SL 5.0pt / TP 10.8pt (live-observed 2026-07-14 trade), conservative SL-first on
ambiguous bars, timeout 2h after entry at close. Cost 0.45pt RT (live net-gross).
Max 1/day Mon-Fri.

Variants:
  A utc1500 : prefix 14:30-15:00 UTC, fire 15:00 UTC (CURRENT LIVE)
  B london  : prefix 14:30-15:00 London local, fire 15:00 London (DST-correct)
  C utc1400 : prefix 13:30-14:00 UTC, fire 14:00 UTC (fixed, = summer fix)
"""
import csv, sys, datetime

BARS = []
with open('/Users/jo/Tick/xau_1m_spliced_2024_2026.csv') as f:
    for row in csv.reader(f):
        BARS.append((int(row[0]), float(row[1]), float(row[2]), float(row[3]), float(row[4])))

SL, TP, COST_RT, CONV = 5.0, 10.8, 0.45, 1.0
TIMEOUT_S = 7200

def last_sunday(year, month):
    d = datetime.date(year, month, 31)
    while d.weekday() != 6: d -= datetime.timedelta(days=1)
    return d

def london_offset(ts):
    dt = datetime.datetime.utcfromtimestamp(ts)
    y = dt.year
    start = datetime.datetime.combine(last_sunday(y,3), datetime.time(1,0))
    end   = datetime.datetime.combine(last_sunday(y,10), datetime.time(1,0))
    return 3600 if start <= dt < end else 0

def run(variant):
    # per-day state
    trades = []
    n = len(BARS)
    i = 0
    day_state = {}  # date -> dict(prefix_open, prefix_close, fired)
    for i in range(n):
        ts, o, h, l, c = BARS[i]
        dt = datetime.datetime.utcfromtimestamp(ts)
        if dt.weekday() >= 5: continue
        if variant == 'A':   fix_utc_min = 15*60
        elif variant == 'C': fix_utc_min = 14*60
        else:                fix_utc_min = 15*60 - london_offset(ts)//60
        mins = dt.hour*60 + dt.minute
        key = dt.date()
        st = day_state.setdefault(key, {'po': None, 'pc': None, 'fired': False})
        if fix_utc_min - 30 <= mins < fix_utc_min:
            if st['po'] is None: st['po'] = o
            st['pc'] = c
            continue
        if mins < fix_utc_min or mins > fix_utc_min + 120: continue
        if st['fired'] or st['po'] is None: continue
        move = st['pc'] - st['po']
        if abs(move) < CONV: continue
        st['fired'] = True
        side = 1 if move > 0 else -1
        entry = o + 0.2*side   # half-spread
        sl_px = entry - side*SL
        tp_px = entry + side*TP
        # walk forward
        pnl = None
        for j in range(i, min(i+TIMEOUT_S//60+1, n)):
            ts2, o2, h2, l2, c2 = BARS[j]
            if ts2 - ts > TIMEOUT_S:
                pnl = side*(c2 - entry); break
            if side > 0:
                if l2 <= sl_px: pnl = -SL; break
                if h2 >= tp_px: pnl = TP; break
            else:
                if h2 >= sl_px: pnl = -SL; break
                if l2 <= tp_px: pnl = TP; break
        if pnl is None:
            pnl = side*(BARS[min(i+TIMEOUT_S//60, n-1)][4] - entry)
        trades.append((ts, side, pnl - COST_RT))
    return trades

def summary(name, trades, lo=None, hi=None):
    t = [x for x in trades if (lo is None or x[0] >= lo) and (hi is None or x[0] < hi)]
    if not t:
        print(f"{name:28s} n=0"); return
    pnls = [x[2] for x in t]
    net = sum(pnls); wins = [p for p in pnls if p > 0]; losses = [p for p in pnls if p <= 0]
    gw = sum(wins); gl = -sum(losses)
    pf = gw/gl if gl > 0 else float('inf')
    wr = len(wins)/len(t)*100
    # max DD
    eq = 0.0; peak = 0.0; dd = 0.0
    for p in pnls:
        eq += p; peak = max(peak, eq); dd = max(dd, peak-eq)
    print(f"{name:28s} n={len(t):4d} net={net:+9.1f}pt  PF={pf:4.2f}  WR={wr:4.1f}%  maxDD={dd:7.1f}pt")

MID = 1750000000  # ~2025-06-15 WF split
for v, label in [('A','A utc1500 (LIVE)'), ('B','B london-local (DST-fix)'), ('C','C utc1400 fixed')]:
    tr = run(v)
    summary(label, tr)
    summary('   WF-1st half', tr, hi=MID)
    summary('   WF-2nd half', tr, lo=MID)
    # summer (BST) subset only: offset 3600
    bst = [x for x in tr if london_offset(x[0]) == 3600]
    gmt = [x for x in tr if london_offset(x[0]) == 0]
    summary('   BST months only', bst)
    summary('   GMT months only', gmt)
    print()
