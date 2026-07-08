#!/usr/bin/env python3
"""gdd_overnight.py -- Study 2: gold overnight-drift anomaly (long overnight, flat intraday).

Split by New York time: INTRADAY = NY 09:30->16:00 approx to nearest H1 bar (10:00->16:00
used on H1 grid; 09:00 variant also tested). OVERNIGHT = 16:00 -> next day's open leg.
Daily-grade: one RT per day. Costs: MGC 0.31pt RT (futures venue) and spot IBKR for reference.
Variants: naked, EMA50-D trend-gated, both directions. No stops -> open-to-open fills.
"""
import sys, datetime as dt
sys.path.insert(0, "/Users/jo/Omega/backtest")
from gdd_lib import load_bars, perf, fmt, NY, xau_spot_cost

def run(path, cost_fn, label, split_pairs, gate=None):
    bars = load_bars(path)
    idx = {}
    for i, b in enumerate(bars):
        d = dt.datetime.fromtimestamp(b["ts"], NY)
        if d.weekday() >= 5: continue
        idx.setdefault(d.date(), {})[d.hour] = i
    days = sorted(idx)
    # daily close series for gate (last bar of NY day)
    dclose = {}
    for d in days:
        h = max(idx[d]); dclose[d] = bars[idx[d][h]]["c"]
    ema = {}; e = None; k = 2 / 51
    for d in days:
        e = dclose[d] if e is None else e + k * (dclose[d] - e)
        ema[d] = e
    out = []
    for (h_start, h_end, nm) in split_pairs:
        for leg in ("overnight", "intraday"):
            for side, sn in ((1, "L"), (-1, "S")):
                tr = []
                for j in range(1, len(days)):
                    d0, d1 = days[j - 1], days[j]
                    if gate == "ema" and dclose[d0] < ema[d0]: continue
                    if leg == "intraday":
                        if h_start not in idx[d1] or h_end not in idx[d1]: continue
                        i0, i1 = idx[d1][h_start], idx[d1][h_end]
                    else:
                        if h_end not in idx[d0] or h_start not in idx[d1]: continue
                        i0, i1 = idx[d0][h_end], idx[d1][h_start]
                    if i1 <= i0: continue
                    px0, px1 = bars[i0]["o"], bars[i1]["o"]
                    c = cost_fn(px0) if callable(cost_fn) else cost_fn
                    tr.append(dict(entry_ts=bars[i0]["ts"], pnl=(px1 - px0) * side - c))
                g = "+EMA50gate" if gate else ""
                out.append(perf(tr, f"{label} {nm} {leg} {sn}{g}"))
    return out

if __name__ == "__main__":
    splits = [(9, 16, "09-16"), (10, 16, "10-16")]
    for m in run("/Users/jo/Tick/XAUUSD_2022_2026.h1.csv", 0.31, "XAU22-26@MGCcost", splits):
        if m["n"] >= 100: print(fmt(m))
    print("--- spot cost ---")
    for m in run("/Users/jo/Tick/XAUUSD_2022_2026.h1.csv", xau_spot_cost, "XAU22-26@spot", [(9, 16, "09-16")]):
        if m["n"] >= 100: print(fmt(m))
    print("--- EMA50-gated (long-only relevance) ---")
    for m in run("/Users/jo/Tick/XAUUSD_2022_2026.h1.csv", 0.31, "XAU22-26@MGCcost", [(9, 16, "09-16")], gate="ema"):
        if m["n"] >= 100: print(fmt(m))
    print("--- MGC native 2024-2026 ---")
    for m in run("/Users/jo/Tick/mgc_2024_2026.h1.csv", 0.31, "MGC24-26", [(9, 16, "09-16")]):
        if m["n"] >= 100: print(fmt(m))
    print("--- 2x cost on the headline ---")
    for m in run("/Users/jo/Tick/XAUUSD_2022_2026.h1.csv", 0.62, "XAU22-26@2xMGC", [(9, 16, "09-16")]):
        if m["n"] >= 100: print(fmt(m))
