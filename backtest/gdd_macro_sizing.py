#!/usr/bin/env python3
"""gdd_macro_sizing.py -- Study 3: macro-score SIZE MULTIPLIER on the faithful XauTF4h stream.

Stream: backtest/xau_tf4h_rider_dump (prod mask 0xC9) on certified XAUUSD_2022_2026.h4.csv.
Score:  2*sign(-d20(DFII10)) + 1*sign(-d20(DTWEXBGS)), lagged 1 business day (publication lag).
Baseline = stream WITH hostile veto (score<=-2 entries skipped) = mirrors the live overlay.
Test: multiplier m on trades entered at score>=+K. Costs are inside the engine pnl and scale
linearly with size, so pnl*m is exact.
"""
import sys, csv, datetime as dt
sys.path.insert(0, "/Users/jo/Omega/backtest")
from gdd_lib import load_fred, perf, fmt, UTC

TRADES = "/private/tmp/claude-501/-Users-jo-Omega/4788ad1b-eefb-447e-8166-4d57aa3c6888/scratchpad/xtf4h_trades_2022_2026.csv"

ry = load_fred("/Users/jo/Tick/DFII10_2015_2026_fred.csv")
dxy = load_fred("/Users/jo/Tick/DTWEXBGS_2015_2026_fred.csv")

def build_score(series_list):
    # daily score dict keyed by date, using each series' own 20-obs slope
    def slopes(s):
        ks = sorted(s)
        out = {}
        for i in range(20, len(ks)):
            diff = s[ks[i]] - s[ks[i - 20]]
            out[ks[i]] = -1 if diff > 0 else (1 if diff < 0 else 0)
        return out
    sry, sdx = slopes(series_list[0]), slopes(series_list[1])
    days = sorted(set(sry) | set(sdx))
    score = {}
    lry = ldx = 0
    for d in days:
        lry = sry.get(d, lry); ldx = sdx.get(d, ldx)
        score[d] = 2 * lry + 1 * ldx
    return score

score_by_day = build_score([ry, dxy])
score_days = sorted(score_by_day)

def score_at(ts):
    # signal available = last macro day STRICTLY BEFORE the entry date (1bd publication lag)
    d = dt.datetime.fromtimestamp(ts, UTC).date()
    # binary search last day < d
    lo, hi = 0, len(score_days)
    while lo < hi:
        m = (lo + hi) // 2
        if score_days[m] < d: lo = m + 1
        else: hi = m
    return score_by_day[score_days[lo - 1]] if lo > 0 else 0

trades = []
with open(TRADES) as f:
    for row in csv.DictReader(f):
        trades.append(dict(entry_ts=int(row["entry_ts"]), pnl=float(row["pnl_usd"]),
                           side=row["side"], engine=row["engine"]))
trades.sort(key=lambda t: t["entry_ts"])
for t in trades:
    t["score"] = score_at(t["entry_ts"])

YRS = [2022, 2023, 2024, 2025, 2026]
print("== raw stream (no macro at all) ==")
print(fmt(perf(trades, "RAW 0xC9 stream")))

# distribution
from collections import Counter
print("score distribution at entry:", sorted(Counter(t["score"] for t in trades).items()))

# per-score bucket economics
print("\n== per-score bucket (raw pnl) ==")
for s in sorted(set(t["score"] for t in trades)):
    b = [t for t in trades if t["score"] == s]
    print(fmt(perf(b, f"score={s:+d}")))

veto = [t for t in trades if t["score"] > -2]
print("\n== baseline: hostile veto (skip score<=-2) ==")
print(fmt(perf(veto, "VETO baseline")))

print("\n== sizing grid on veto baseline: pnl*m when score>=K ==")
for K in (1, 2, 3):
    for m in (1.25, 1.5, 2.0):
        sized = [dict(entry_ts=t["entry_ts"],
                      pnl=t["pnl"] * (m if t["score"] >= K else 1.0)) for t in veto]
        print(fmt(perf(sized, f"K>=+{K} m={m}")))

# same grid but WITHOUT the hostile veto (in case live path lacks it)
print("\n== sizing grid on RAW stream (no veto) ==")
for K in (2,):
    for m in (1.5, 2.0):
        sized = [dict(entry_ts=t["entry_ts"],
                      pnl=t["pnl"] * (m if t["score"] >= K else 1.0)) for t in trades]
        print(fmt(perf(sized, f"RAW K>=+{K} m={m}")))

# yearly detail for the headline config
print("\n== yearly: veto vs veto+K2m2 ==")
for lbl, tl in [("veto 1x", veto),
                ("veto K2 m2", [dict(entry_ts=t["entry_ts"], pnl=t["pnl"] * (2.0 if t["score"] >= 2 else 1.0)) for t in veto])]:
    m = perf(tl, lbl, years=YRS)
    print(fmt(m), " | " + " ".join(f"{y}:{m[f'y{y}']:+.0f}" for y in YRS))
