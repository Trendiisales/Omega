#!/usr/bin/env python3
"""gold_tom_cost_recheck.py -- GOLD PHASE 1 bug 4 companion check (2026-07-11).

CalendarTom XAU instance: re-run the TOM rule (long last3+first3 trading days,
open-to-open, weekday approximation -- the engine's own calendar logic) on GC_F
daily 2015-2026 at the honest spot cost grid (2bp original basis, 6bp+1/2/4bp
slip, 16bp stress). PnL in bp of entry per trade (unit-notional book, matching
the tom_backtest.py convention).
"""
import sys, datetime as dt
sys.path.insert(0, "/Users/jo/Omega/backtest")
from gdd_lib import load_daily_yahoo

gold = load_daily_yahoo("/Users/jo/Tick/GC_F_daily_2016_2026_yahoo.csv")
LAST_N = FIRST_N = 3

def is_weekday(d): return d.weekday() < 5

def days_in_month(y, m):
    if m == 12: return 31
    return (dt.date(y, m + 1, 1) - dt.date(y, m, 1)).days

def is_tom_day(d):
    if not is_weekday(d): return False
    tdom = sum(1 for k in range(1, d.day + 1) if is_weekday(d.replace(day=k)))
    if tdom <= FIRST_N: return True
    dim = days_in_month(d.year, d.month)
    bde = sum(1 for k in range(d.day, dim + 1) if is_weekday(d.replace(day=k)))
    return bde <= LAST_N

# entry at OPEN of first in-window day, exit at OPEN of first out-window day
trades = []  # (entry_date, ret_frac)
pos_open = None
for (d, o, h, l, c) in gold:
    inw = is_tom_day(d)
    if pos_open is None and inw:
        pos_open = (d, o)
    elif pos_open is not None and not inw:
        trades.append((pos_open[0], (o - pos_open[1]) / pos_open[1]))
        pos_open = None

def report(label, cost_bp):
    pn = [r * 1e4 - cost_bp for (_, r) in trades]   # bp per trade net
    net = sum(pn)
    g = sum(x for x in pn if x > 0); l = -sum(x for x in pn if x < 0)
    pf = g / l if l > 0 else float("inf")
    mid = len(pn) // 2
    y22 = sum(x for ((d, _), x) in zip(trades, pn) if d.year == 2022)
    print(f"{label:<28s} n={len(pn):<4d} net={net:+8.1f}bp PF={pf:4.2f} "
          f"H1={sum(pn[:mid]):+7.1f} H2={sum(pn[mid:]):+7.1f} 2022={y22:+7.1f} "
          f"avg={net/len(pn):+5.1f}bp/trade")

print(f"XAU TOM last{LAST_N}+first{FIRST_N} open-to-open, GC_F daily "
      f"{gold[0][0]}..{gold[-1][0]}, n={len(trades)} windows")
report("2bp RT (validation basis)", 2.0)
report("spot 7bp RT (6+1 slip)",    7.0)
report("spot 8bp RT (6+2 slip)",    8.0)
report("spot 10bp RT (6+4 slip)",   10.0)
report("16bp RT (2x stress)",       16.0)
