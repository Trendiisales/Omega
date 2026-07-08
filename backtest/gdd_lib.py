#!/usr/bin/env python3
"""gdd_lib.py -- shared loaders/metrics for the 2026-07-08 gold deep-dive (research-only).

Data provenance:
  /Users/jo/Tick/XAUUSD_2022_2026.h1.csv|.h4.csv   certified 2026-07-08 (data_integrity_gate)
  /Users/jo/Tick/mgc_30m_hist.csv, mgc_2024_2026.h1.csv  certified 2026-07-08
  /Users/jo/Tick/xag/XAGUSD_h1_clean.csv  gate REJECTED on 3x-median heuristic; Jan-2026
      silver squeeze cross-verified GENUINE vs independent SI=F yahoo daily (121.30 high
      2026-01-29) -> used with documented override, results reported incl/excl squeeze.
  /Users/jo/Tick/GC_F_daily_2016_2026_yahoo.csv etc  yfinance fetch 2026-07-08 (daily closes).
  /Users/jo/Tick/DFII10_2015_2026_fred.csv, DTWEXBGS_2015_2026_fred.csv  FRED fetch 2026-07-08.
  /Users/jo/Omega/cot/cot_2019..2026.txt  CFTC legacy futures-only, local cache.
"""
import csv, datetime as dt, math
from zoneinfo import ZoneInfo

UTC = ZoneInfo("UTC"); LON = ZoneInfo("Europe/London"); NY = ZoneInfo("America/New_York")

def load_bars(path, ts_col=0, has_header=True):
    """ts(sec),o,h,l,c[,v] -> list of dicts sorted by ts."""
    out = []
    with open(path) as f:
        r = csv.reader(f)
        if has_header: next(r)
        for p in r:
            if len(p) < 5: continue
            try:
                out.append(dict(ts=int(float(p[0])), o=float(p[1]), h=float(p[2]),
                                l=float(p[3]), c=float(p[4])))
            except ValueError:
                continue
    out.sort(key=lambda b: b["ts"])
    return out

def load_daily_yahoo(path):
    """yahoo csv Date,Open,High,Low,Close -> [(date,o,h,l,c)]"""
    out = []
    with open(path) as f:
        r = csv.reader(f); next(r)
        for p in r:
            if len(p) < 5 or not p[1]: continue
            try:
                d = dt.date.fromisoformat(p[0][:10])
                out.append((d, float(p[1]), float(p[2]), float(p[3]), float(p[4])))
            except ValueError:
                continue
    out.sort()
    return out

def load_fred(path):
    out = {}
    with open(path) as f:
        r = csv.reader(f); next(r)
        for p in r:
            if len(p) < 2 or p[1] in ("", "."): continue
            out[dt.date.fromisoformat(p[0])] = float(p[1])
    return out

# ---------- costs ----------
def xau_spot_cost(px):   # IBKR spot gold round-trip, $/oz-equivalent points
    return 2 * 0.00015 * px + 0.30
def mgc_cost():          # MGC futures round-trip in points
    return 0.31
def xag_spot_cost(px):   # silver spot RT points (commission 2*0.025% + ~3c spread)
    return 2 * 0.00025 * px + 0.03

# ---------- metrics ----------
def perf(trades, label="", years=None):
    """trades: list of dicts with pnl (points or $) and entry_ts (sec). Returns metric dict."""
    if not trades:
        return dict(label=label, n=0, net=0.0, pf=0.0, win=0.0, maxdd=0.0, h1=0.0, h2=0.0,
                    y2022=0.0, exbest=0.0)
    pn = [t["pnl"] for t in trades]
    net = sum(pn)
    g = sum(x for x in pn if x > 0); l = -sum(x for x in pn if x < 0)
    pf = g / l if l > 0 else float("inf")
    win = sum(1 for x in pn if x > 0) / len(pn)
    eq = 0.0; peak = 0.0; mdd = 0.0
    for x in pn:
        eq += x; peak = max(peak, eq); mdd = min(mdd, eq - peak)
    ts = sorted(t["entry_ts"] for t in trades)
    mid = ts[len(ts) // 2]
    h1 = sum(t["pnl"] for t in trades if t["entry_ts"] < mid)
    h2 = net - h1
    y22 = sum(t["pnl"] for t in trades if dt.datetime.fromtimestamp(t["entry_ts"], UTC).year == 2022)
    exbest = net - max(pn)
    d = dict(label=label, n=len(pn), net=net, pf=pf, win=win, maxdd=mdd, h1=h1, h2=h2,
             y2022=y22, exbest=exbest)
    if years:
        for y in years:
            d[f"y{y}"] = sum(t["pnl"] for t in trades
                             if dt.datetime.fromtimestamp(t["entry_ts"], UTC).year == y)
    return d

def fmt(m):
    return (f"{m['label']:<44s} n={m['n']:<4d} net={m['net']:>+9.1f} PF={m['pf']:>5.2f} "
            f"win={m['win']*100:>4.1f}% dd={m['maxdd']:>+8.1f} h1={m['h1']:>+8.1f} h2={m['h2']:>+8.1f} "
            f"2022={m['y2022']:>+8.1f} exb={m['exbest']:>+8.1f}")

def atr_series(bars, n=14):
    """simple ATR (Wilder) on bar list; returns list aligned to bars (None until warm)."""
    out = [None] * len(bars); prev_c = None; a = None
    for i, b in enumerate(bars):
        tr = b["h"] - b["l"] if prev_c is None else max(b["h"] - b["l"], abs(b["h"] - prev_c), abs(b["l"] - prev_c))
        a = tr if a is None else (a * (n - 1) + tr) / n
        if i >= n: out[i] = a
        prev_c = b["c"]
    return out
