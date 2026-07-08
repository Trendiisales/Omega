#!/usr/bin/env python3
"""gdd_silver.py -- Study 5: gold->silver lead-lag + GSR z-extreme MR pair.

Data: XAUUSD_2022_2026.h1.csv (certified) x xag/XAGUSD_h1_clean.csv (gate-override:
Jan-2026 squeeze verified genuine vs SI=F; results also reported EXCL 2026-01-15..2026-03-31).
Daily GSR long-history check: GC=F / SI=F yahoo 2015-2026.
Costs: silver spot 2*0.00025*px+0.03; gold spot 2*0.00015*px+0.30; pair = both legs.
Fills: next-bar-open entries/exits; timeout exits at close; no stops (MR sized small).
"""
import sys, datetime as dt, statistics
sys.path.insert(0, "/Users/jo/Omega/backtest")
from gdd_lib import load_bars, load_daily_yahoo, perf, fmt, xau_spot_cost, xag_spot_cost

xau = load_bars("/Users/jo/Tick/XAUUSD_2022_2026.h1.csv")
xag = load_bars("/Users/jo/Tick/xag/XAGUSD_h1_clean.csv")
gmap = {b["ts"]: i for i, b in enumerate(xau)}
smap = {b["ts"]: i for i, b in enumerate(xag)}
common = sorted(set(gmap) & set(smap))
print(f"# overlap H1 bars: {len(common)}  {dt.datetime.utcfromtimestamp(common[0]).date()}..{dt.datetime.utcfromtimestamp(common[-1]).date()}")

SQ0 = dt.date(2026, 1, 15); SQ1 = dt.date(2026, 3, 31)
def in_squeeze(ts):
    d = dt.datetime.utcfromtimestamp(ts).date()
    return SQ0 <= d <= SQ1

# ---- (a) gold jump -> silver follow ----
def jump_follow(z_th=2.0, look=4, hold=24, side_mode="follow", excl_squeeze=False):
    rets = []
    tr = []
    for ci in range(200, len(common) - hold - 1):
        ts = common[ci]
        gi = gmap[ts]
        if gi < look + 200: continue
        r = xau[gi]["c"] / xau[gi - look]["c"] - 1
        w = [xau[j]["c"] / xau[j - look]["c"] - 1 for j in range(gi - 200, gi)]
        sd = statistics.pstdev(w) or 1e-9; mu = statistics.mean(w)
        z = (r - mu) / sd
        if abs(z) < z_th: continue
        if excl_squeeze and in_squeeze(ts): continue
        side = (1 if z > 0 else -1) * (1 if side_mode == "follow" else -1)
        si_e = smap[common[ci + 1]]; si_x = smap[common[min(ci + 1 + hold, len(common) - 1)]]
        e, x = xag[si_e]["o"], xag[si_x]["o"]
        tr.append(dict(entry_ts=ts, pnl=(x - e) * side - xag_spot_cost(e)))
        # skip overlapping signals
    # dedupe overlapping: keep first per hold window
    ded = []; last = -1
    for t in tr:
        if t["entry_ts"] - last >= hold * 3600 * 0.5:
            ded.append(t); last = t["entry_ts"]
    return ded

print("\n== (a) gold H1 jump z -> trade SILVER (hold in H1 bars) ==")
for zt in (2.0, 2.5):
    for hold in (12, 24, 48):
        for mode in ("follow", "fade"):
            m = perf(jump_follow(zt, 4, hold, mode), f"jump z>{zt} hold{hold} {mode}")
            if m["n"] >= 25: print(fmt(m))
print("-- excl squeeze window --")
for zt in (2.0,):
    for hold in (24, 48):
        m = perf(jump_follow(zt, 4, hold, "follow", excl_squeeze=True), f"jump z>{zt} hold{hold} follow XSQ")
        if m["n"] >= 25: print(fmt(m))

# ---- (b) gold trend -> hold silver ----
def trend_hold(fast=20, slow=100, excl_squeeze=False):
    ef = es = None; kf = 2 / (fast + 1); ks = 2 / (slow + 1)
    tr = []; pos = None
    for ci in range(len(common)):
        gi = gmap[common[ci]]
        c = xau[gi]["c"]
        ef = c if ef is None else ef + kf * (c - ef)
        es = c if es is None else es + ks * (c - es)
        if ci < slow: continue
        want = 1 if ef > es else 0
        if excl_squeeze and in_squeeze(common[ci]): want = 0
        if pos is None and want == 1 and ci + 1 < len(common):
            si = smap[common[ci + 1]]
            pos = dict(e=xag[si]["o"], ts=common[ci + 1])
        elif pos is not None and want == 0 and ci + 1 < len(common):
            si = smap[common[ci + 1]]
            tr.append(dict(entry_ts=pos["ts"], pnl=(xag[si]["o"] - pos["e"]) - xag_spot_cost(pos["e"])))
            pos = None
    return tr

print("\n== (b) gold H1 EMA trend -> hold SILVER long ==")
for f, s in ((20, 100), (50, 200)):
    print(fmt(perf(trend_hold(f, s), f"goldEMA{f}/{s} -> silver L")))
    print(fmt(perf(trend_hold(f, s, True), f"goldEMA{f}/{s} -> silver L XSQ")))
# control: silver's own trend
def silver_own(f=20, s=100):
    ef = es = None; kf = 2 / (f + 1); ks = 2 / (s + 1)
    tr = []; pos = None
    for ci in range(len(common)):
        si = smap[common[ci]]
        c = xag[si]["c"]
        ef = c if ef is None else ef + kf * (c - ef)
        es = c if es is None else es + ks * (c - es)
        if ci < s: continue
        want = 1 if ef > es else 0
        if pos is None and want and ci + 1 < len(common):
            pos = dict(e=xag[smap[common[ci + 1]]]["o"], ts=common[ci + 1])
        elif pos and not want and ci + 1 < len(common):
            tr.append(dict(entry_ts=pos["ts"], pnl=(xag[smap[common[ci + 1]]]["o"] - pos["e"]) - xag_spot_cost(pos["e"])))
            pos = None
    return tr
print(fmt(perf(silver_own(), "CONTROL silver own EMA20/100 L")))

# ---- (c) GSR z-extreme MR pair (daily, 10yr yahoo) ----
gc = load_daily_yahoo("/Users/jo/Tick/GC_F_daily_2016_2026_yahoo.csv")
si = load_daily_yahoo("/Users/jo/Tick/SI_F_daily_2016_2026_yahoo.csv")
gd = {r[0]: r[4] for r in gc}; sd = {r[0]: r[4] for r in si}
days = sorted(set(gd) & set(sd))
ratio = [gd[d] / sd[d] for d in days]
print(f"\n== (c) GSR daily MR, {len(days)} days {days[0]}..{days[-1]} ==")
def gsr_mr(z_in=2.0, z_out=0.5, lb=120, timeout=40, notional=1000.0):
    tr = []; pos = None
    for t in range(lb, len(days)):
        w = ratio[t - lb:t]
        mu = statistics.mean(w); sdv = statistics.pstdev(w) or 1e-9
        z = (ratio[t] - mu) / sdv
        d = days[t]
        if pos:
            done = (pos["side"] == 1 and z >= -z_out) or (pos["side"] == -1 and z <= z_out) or (t - pos["t0"] >= timeout)
            if done:
                # side=+1 = long ratio = long gold/short silver; pnl in $ per $notional/leg
                g_ret = gd[d] / pos["g"] - 1; s_ret = sd[d] / pos["s"] - 1
                pnl = pos["side"] * (g_ret - s_ret) * notional
                cost = (xau_spot_cost(pos["g"]) / pos["g"] + xag_spot_cost(pos["s"]) / pos["s"]) * notional
                tr.append(dict(entry_ts=int(dt.datetime.combine(pos["d"], dt.time()).timestamp()),
                               pnl=pnl - cost))
                pos = None
        if pos is None:
            if z > z_in:  pos = dict(side=-1, g=gd[d], s=sd[d], d=d, t0=t)   # ratio high: short gold/long silver
            elif z < -z_in: pos = dict(side=1, g=gd[d], s=sd[d], d=d, t0=t)  # ratio low: long gold/short silver
    return tr
for zi in (1.5, 2.0, 2.5):
    for lb in (60, 120, 250):
        m = perf(gsr_mr(zi, 0.5, lb), f"GSR z>{zi} lb{lb} $1k/leg")
        if m["n"] >= 15: print(fmt(m))
