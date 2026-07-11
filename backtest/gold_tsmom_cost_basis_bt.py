#!/usr/bin/env python3
"""gold_tsmom_cost_basis_bt.py -- GOLD PHASE 1 bug 3 (2026-07-11).

GoldTsmomD1V2 ships with cost_rt_pts=0.31 (MGC futures basis) but runs as an
XAUUSD SPOT book (engine_init lot=0.01 XAU spot, tr.symbol=XAUUSD). Re-run the
WIRED rule (calendar-month rebalance, comp{42,63,84}, lev=min(2,0.15/rv21),
pnl_pts = w*(close-anchor) - |dw|*cost/2) on GC_F daily 2015-2026 at BOTH cost
bases per /Users/jo/Memory-Omega/GOLD_BOOK_ROADMAP.md:
  (a) true spot cost: 6bp base + {1,2,4}bp slippage, charged as price-
      proportional pts per unit turnover at the rebalance close
      (+ the gdd_lib xau_spot_cost IBKR formula 2*1.5bp*px + $0.30 spread)
  (b) futures basis: MGC 0.31pt RT (current wired constant), GC ~0.25pt,
      and 2x stress of each.
Metrics per the engine's own history: net pts, PF, WF halves, 2022 window,
maxDD, n. Data: /Users/jo/Tick/GC_F_daily_2016_2026_yahoo.csv (same file the
Study-4 validation used).
"""
import sys, datetime as dt, statistics
sys.path.insert(0, "/Users/jo/Omega/backtest")
from gdd_lib import load_daily_yahoo

gold = load_daily_yahoo("/Users/jo/Tick/GC_F_daily_2016_2026_yahoo.csv")
dates = [g[0] for g in gold]; close = [g[4] for g in gold]
N = len(gold)
LBS = (42, 63, 84)

def run(cost_fn):
    """cost_fn(px) -> RT cost in points per unit turnover. Wired rule:
    rebalance at the close of the FIRST trading day of each calendar month."""
    trades = []
    w = 0.0; anchor = None; anchor_d = None
    last_m = None
    for t in range(1, N):
        d = dates[t]
        month_flip = last_m is not None and (d.month, d.year) != last_m
        last_m = (d.month, d.year)
        if not month_flip: continue
        if t < 85: continue                      # need lb84 history
        comp = sum((1 if close[t] > close[t - lb] else -1) for lb in LBS) / 3.0
        rets = [(close[i] / close[i - 1] - 1) for i in range(t - 20, t + 1)]
        rv = statistics.pstdev(rets) * (252 ** 0.5) or 1e-9
        want = comp * min(2.0, 0.15 / rv)
        if anchor is not None and (w != 0.0 or abs(want - w) > 1e-9):
            pnl = w * (close[t] - anchor) - abs(want - w) * cost_fn(close[t]) / 2.0
            trades.append((anchor_d, d, w, pnl))
        w = want; anchor = close[t]; anchor_d = d
    return trades

def report(label, trades):
    pn = [x[3] for x in trades]
    if not pn:
        print(f"{label:<38s} n=0"); return
    net = sum(pn)
    g = sum(x for x in pn if x > 0); l = -sum(x for x in pn if x < 0)
    pf = g / l if l > 0 else float("inf")
    mid = len(pn) // 2
    h1, h2 = sum(pn[:mid]), sum(pn[mid:])
    y22 = sum(p for (a, b, w, p) in trades if a.year == 2022)
    eq = peak = mdd = 0.0
    for x in pn:
        eq += x; peak = max(peak, eq); mdd = min(mdd, eq - peak)
    print(f"{label:<38s} n={len(pn):<4d} net={net:+8.1f}pt PF={pf:4.2f} "
          f"H1={h1:+8.1f} H2={h2:+8.1f} 2022={y22:+7.1f} maxDD={mdd:8.1f}")

print(f"data: {dates[0]}..{dates[-1]} n_days={N}")
print("== GoldTsmomD1V2 wired rule (calendar-month, comp{42,63,84}, volT15) ==")
print("-- (b) futures cost basis (points RT per unit turnover) --")
report("MGC 0.31pt (CURRENT wired)",        run(lambda px: 0.31))
report("MGC 0.62pt (2x stress)",            run(lambda px: 0.62))
report("GC  0.25pt",                        run(lambda px: 0.25))
print("-- (a) true XAU-spot cost basis --")
report("spot 6bp+1bp slip (7bp RT)",        run(lambda px: px * 7e-4))
report("spot 6bp+2bp slip (8bp RT)",        run(lambda px: px * 8e-4))
report("spot 6bp+4bp slip (10bp RT)",       run(lambda px: px * 10e-4))
report("spot 16bp RT (2x stress of 8bp)",   run(lambda px: px * 16e-4))
report("IBKR fml 2*1.5bp*px+0.30",          run(lambda px: 2 * 0.00015 * px + 0.30))
print("-- context: zero cost --")
report("zero cost",                         run(lambda px: 0.0))
