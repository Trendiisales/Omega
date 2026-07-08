#!/usr/bin/env python3
"""gdd_tsmom_cot.py -- Study 4 (vol-targeted TSMOM, GC=F daily 2015-2026) +
Study 6 (COT net-noncommercial percentile overlay).

TSMOM: composite sign of {21,63,126,252}d returns, position = comp/4 * min(2, tgt/realized20),
monthly (21d) rebalance, cost 0.31pt MGC per unit turnover. Long-only, short-only, both.
COT: legacy futures-only "GOLD - COMMODITY EXCHANGE INC" net noncomm %OI, 3yr rolling
percentile, applied with 3-business-day release lag. Forward 5d gold return by decile +
overlay on TSMOM (veto/flip at extremes).
"""
import sys, csv, datetime as dt, statistics
sys.path.insert(0, "/Users/jo/Omega/backtest")
from gdd_lib import load_daily_yahoo, perf, fmt

gold = load_daily_yahoo("/Users/jo/Tick/GC_F_daily_2016_2026_yahoo.csv")
dates = [g[0] for g in gold]; close = [g[4] for g in gold]
N = len(gold)

def tsmom_run(mode="both", tgt_ann=0.15, cost=0.31, lbs=(21, 63, 126, 252), label=""):
    pos = 0.0; trades = []; cur = None
    for t in range(253, N - 1):
        if (t - 253) % 21 == 0:  # monthly rebalance
            comp = sum((1 if close[t] > close[t - lb] else -1) for lb in lbs) / len(lbs)
            rets = [(close[i] / close[i - 1] - 1) for i in range(t - 20, t + 1)]
            rv = statistics.pstdev(rets) * (252 ** 0.5) or 1e-9
            lev = min(2.0, tgt_ann / rv)
            want = comp * lev
            if mode == "long": want = max(0.0, want)
            if mode == "short": want = min(0.0, want)
            if cur is None or abs(want - cur["w"]) > 1e-9:
                if cur is not None:
                    pnl = cur["w"] * (close[t] - cur["px"]) - abs(cur["w"] - want) * cost / 2 - abs(cur["w"]) * 0  # cost charged on entry below
                    trades.append(dict(entry_ts=int(dt.datetime.combine(cur["d"], dt.time()).timestamp()),
                                       pnl=cur["w"] * (close[t] - cur["px"]) - abs(want - cur["w"]) * cost / 2))
                cur = dict(w=want, px=close[t], d=dates[t])
    return trades

print("== Study 4: vol-targeted TSMOM GC=F daily (21d rebalance, cost 0.31/turnover) ==")
for mode in ("both", "long"):
    m = perf(tsmom_run(mode), f"TSMOM comp4 {mode} volT15")
    print(fmt(m))
for lbs, nm in (((252,), "12m only"), ((63,), "3m only"), ((21,), "1m only")):
    print(fmt(perf(tsmom_run("both", lbs=lbs), f"TSMOM {nm} both")))
print(fmt(perf(tsmom_run("both", cost=0.62), "TSMOM comp4 both 2xCOST")))

# ---------------- COT ----------------
def load_cot():
    seen = {}
    for y in range(2019, 2027):
        try:
            txt = open(f"/Users/jo/Omega/cot/cot_{y}.txt").read()
        except FileNotFoundError:
            continue
        for ln in txt.splitlines():
            if not ln.startswith('"GOLD - COMMODITY EXCHANGE INC.'): continue
            p = next(csv.reader([ln]))
            try:
                d = dt.date.fromisoformat(p[2]); oi = float(p[7])
                netp = (float(p[8]) - float(p[9])) / oi if oi else 0.0
            except (ValueError, IndexError):
                continue
            if d not in seen: seen[d] = netp
    return dict(sorted(seen.items()))

cot = load_cot()
ks = sorted(cot)
print(f"\n== Study 6: COT gold net-noncomm %OI, {len(ks)} weeks {ks[0]}..{ks[-1]} ==")
# percentile within trailing 156 weeks, lagged 3bd
pctl = {}
for i, d in enumerate(ks):
    if i < 52: continue
    hist = sorted(cot[k] for k in ks[max(0, i - 156):i + 1])
    r = sum(1 for v in hist if v <= cot[d]) / len(hist)
    pctl[d + dt.timedelta(days=3)] = r  # release lag
# map to daily
pk = sorted(pctl)
def pct_at(d):
    lo, hi = 0, len(pk)
    while lo < hi:
        m = (lo + hi) // 2
        if pk[m] <= d: lo = m + 1
        else: hi = m
    return pctl[pk[lo - 1]] if lo > 0 else None

# forward 5d return by COT quintile
buckets = {}
for t in range(0, N - 5):
    p = pct_at(dates[t])
    if p is None: continue
    q = min(4, int(p * 5))
    buckets.setdefault(q, []).append((close[t + 5] / close[t] - 1) * 100)
print("quintile: n, mean fwd-5d %, median")
for q in sorted(buckets):
    b = buckets[q]
    print(f"  Q{q + 1}  n={len(b):<4d} mean={statistics.mean(b):+.3f}%  med={statistics.median(b):+.3f}%")

# overlay: TSMOM both, veto longs when COT pctl>0.9 (crowded), veto shorts when <0.1
base = tsmom_run("both")
def cot_overlay(th_hi=0.9, th_lo=0.1):
    out = []
    for tr in base:
        d = dt.datetime.fromtimestamp(tr["entry_ts"]).date()
        p = pct_at(d)
        if p is None: out.append(tr); continue
        out.append(tr if not ((tr["pnl"] != 0) and ((p > th_hi and tr["pnl"] > -1e18 and True) is False)) else tr)
    return out
# simpler: recompute tsmom with cot veto inline
def tsmom_cot(th_hi=0.9, th_lo=0.1, cost=0.31):
    trades = []; cur = None
    for t in range(253, N - 1):
        if (t - 253) % 21 == 0:
            comp = sum((1 if close[t] > close[t - lb] else -1) for lb in (21, 63, 126, 252)) / 4
            rets = [(close[i] / close[i - 1] - 1) for i in range(t - 20, t + 1)]
            rv = statistics.pstdev(rets) * (252 ** 0.5) or 1e-9
            want = comp * min(2.0, 0.15 / rv)
            p = pct_at(dates[t])
            if p is not None:
                if want > 0 and p > th_hi: want = 0.0   # crowded long -> stand down
                if want < 0 and p < th_lo: want = 0.0   # crowded short -> stand down
            if cur is not None:
                trades.append(dict(entry_ts=int(dt.datetime.combine(cur["d"], dt.time()).timestamp()),
                                   pnl=cur["w"] * (close[t] - cur["px"]) - abs(want - cur["w"]) * cost / 2))
            cur = dict(w=want, px=close[t], d=dates[t])
    return trades
print("\nTSMOM + COT crowding veto:")
print(fmt(perf(base, "TSMOM base (again)")))
for hi, lo in ((0.9, 0.1), (0.85, 0.15), (0.95, 0.05)):
    print(fmt(perf(tsmom_cot(hi, lo), f"TSMOM+COTveto {hi}/{lo}")))
