#!/usr/bin/env python3
"""gdd_london_fix.py -- Study 1: London AM/PM fix drift & reversion on MGC 30m (futures costs).

Mechanism: documented institutional flow around the 10:30 (AM) and 15:00 (PM) London fixes.
Test grid: hold windows entering T-minutes before fix, exiting at/after fix, both directions;
plus post-fix reversion conditional on the pre-fix move sign.
Data: mgc_30m_hist.csv (certified, 2024-06..2026-06). 2022 shadow-check runs separately on
XAU M30 built from xau_m1_2022bear.csv (spot prices, MGC costs as venue proxy -- caveat noted).
Fills: bar-open to bar-open (no stops -> no intrabar issue). Cost 0.31pt RT (2x = 0.62).
"""
import sys, datetime as dt
sys.path.insert(0, "/Users/jo/Omega/backtest")
from gdd_lib import load_bars, perf, fmt, LON, UTC

def london_hm(ts):
    d = dt.datetime.fromtimestamp(ts, LON)
    return d.hour * 60 + d.minute, d.date(), d.weekday()

def run(bars, cost, label_prefix=""):
    # index bars by (date, london-minute)
    by_day = {}
    for i, b in enumerate(bars):
        hm, d, wd = london_hm(b["ts"])
        if wd >= 5: continue
        by_day.setdefault(d, {})[hm] = i
    res = []
    def window_trades(ent_hm, ex_hm, side):
        tr = []
        for d, mp in by_day.items():
            if ent_hm not in mp or ex_hm not in mp: continue
            i0, i1 = mp[ent_hm], mp[ex_hm]
            if i1 <= i0: continue
            pts = (bars[i1]["o"] - bars[i0]["o"]) * side - cost
            tr.append(dict(entry_ts=bars[i0]["ts"], pnl=pts))
        return tr
    AM, PM = 10 * 60 + 30, 15 * 60
    grids = []
    for fix, nm in ((AM, "AMfix"), (PM, "PMfix")):
        for pre in (60, 90, 120):
            for side, sn in ((1, "L"), (-1, "S")):
                grids.append((f"{nm} drift-in {pre}m {sn}", fix - pre, fix, side))
        for post in (60, 90, 120):
            for side, sn in ((1, "L"), (-1, "S")):
                grids.append((f"{nm} post {post}m {sn}", fix, fix + post, side))
    out = []
    for lbl, e, x, s in grids:
        m = perf(window_trades(e, x, s), label_prefix + lbl)
        out.append(m)
    # conditional post-fix reversion: fade the T-120m..fix move after the fix (exit +90m)
    for fix, nm in ((AM, "AMfix"), (PM, "PMfix")):
        tr = []
        for d, mp in by_day.items():
            if (fix - 120) not in mp or fix not in mp or (fix + 90) not in mp: continue
            pre_mv = bars[mp[fix]]["o"] - bars[mp[fix - 120]]["o"]
            if pre_mv == 0: continue
            side = -1 if pre_mv > 0 else 1
            pts = (bars[mp[fix + 90]]["o"] - bars[mp[fix]]["o"]) * side - cost
            tr.append(dict(entry_ts=bars[mp[fix]]["ts"], pnl=pts))
        out.append(perf(tr, label_prefix + f"{nm} fade-prefix-move +90m"))
        tr2 = [dict(entry_ts=t["entry_ts"], pnl=-(t["pnl"] + cost) - cost) for t in tr]
        out.append(perf(tr2, label_prefix + f"{nm} follow-prefix-move +90m"))
    return out

if __name__ == "__main__":
    path = sys.argv[1] if len(sys.argv) > 1 else "/Users/jo/Tick/mgc_30m_hist.csv"
    cost = float(sys.argv[2]) if len(sys.argv) > 2 else 0.31
    bars = load_bars(path)
    print(f"# {path}  n={len(bars)} cost={cost}")
    for m in run(bars, cost):
        if m["n"] >= 30: print(fmt(m))
