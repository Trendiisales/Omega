#!/usr/bin/env python3
"""
macro_gold_regime.py -- backtest the MacroGoldRegime BIAS/SIZE-TILT overlay (build #1).

What it tests
-------------
Winners trade gold off the macro regime: real yields (dominant, inverse), the dollar
(inverse), and COT positioning extremes (contrarian). Omega has NO exogenous macro input
(RegimeState is price-only). This harness measures whether a simple, pre-committed macro
score, used as a directional BIAS / SIZE TILT on a baseline gold trend, improves
risk-adjusted return and cuts drawdown vs the bare trend.

Honesty / BACKTEST_TRUTH notes
------------------------------
* DAILY close-to-close overlay. Signal computed on data through close[t]; position held
  over t->t+1. No intrabar trailing => the bar-replay within-bar look-ahead disease does
  NOT apply here. This is still a discovery-grade study, not an engine-faithful tick BT.
* COT is lagged to its real release (Tuesday data published the following Friday) -> we
  apply it with a 3-business-day delay, then forward-fill. No look-ahead.
* Corpus 2024-03..2026-04 is ONE gold-BULL regime (2044 -> 4710). Long-only trend will
  look great on beta alone. The real question is risk-adjusted (Sharpe / maxDD), NOT net.
* Cost-honest: round-trip cost charged on every change in position size (turnover-weighted).
"""
import csv, datetime as dt, statistics as st, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BEAR = "--bear" in sys.argv
if BEAR:
    GOLD = ROOT / "data/macro/gold_bear_daily.csv"
    DFII10 = ROOT / "data/macro/DFII10_bear.csv"
    DXY = ROOT / "data/macro/DXY_bear.csv"
else:
    GOLD = Path("/Users/jo/Tick/2yr_XAUUSD_daily.csv")
    DFII10 = ROOT / "data/macro/DFII10.csv"
    DXY = ROOT / "data/macro/DTWEXBGS.csv"
COT_DIR = ROOT / "cot"

COST_BPS_RT = 3.0     # round-trip cost in bps of notional (IBKR gold ~0.37pt + spread, conservative)
LB_TREND = 60         # trend lookback (business days) for TSMOM baseline
LB_SLOPE = 20         # macro slope lookback (business days)

# ---------- loaders ----------
def load_gold():
    rows = []
    for ln in GOLD.read_text().splitlines():
        p = ln.split(",")
        if len(p) < 5: continue
        d = dt.date(int(p[0][:4]), int(p[0][4:6]), int(p[0][6:8]))
        rows.append((d, float(p[1]), float(p[2]), float(p[3]), float(p[4])))
    rows.sort()
    return rows  # (date,o,h,l,c)

def load_fred(path):
    out = {}
    for i, ln in enumerate(path.read_text().splitlines()):
        if i == 0: continue
        d, v = ln.split(",")
        if v in ("", "."): continue
        out[dt.date.fromisoformat(d)] = float(v)
    return out

def load_cot_gold():
    # net noncommercial (specs) position; dedupe by date (files carry futures-only + combined)
    seen = {}
    for y in (2024, 2025, 2026):
        f = COT_DIR / f"cot_{y}.txt"
        if not f.exists(): continue
        for ln in f.read_text().splitlines():
            if not ln.startswith('"GOLD - COMMODITY EXCHANGE INC.'): continue
            parts = next(csv.reader([ln]))
            try:
                d = dt.date.fromisoformat(parts[2])
                oi = float(parts[7]); nc_long = float(parts[8]); nc_short = float(parts[9])
            except (ValueError, IndexError):
                continue
            net_pct = (nc_long - nc_short) / oi if oi else 0.0
            if d not in seen:      # first occurrence wins (consistent report)
                seen[d] = net_pct
    return dict(sorted(seen.items()))

def ffill_to(dates, series):
    """forward-fill a sparse {date:val} onto an ordered date list."""
    keys = sorted(series); out = []; j = 0; last = None
    for d in dates:
        while j < len(keys) and keys[j] <= d:
            last = series[keys[j]]; j += 1
        out.append(last)
    return out

# ---------- metrics ----------
def metrics(rets):
    rets = [r for r in rets if r is not None]
    if not rets: return dict(n=0, ret=0, pf=0, sharpe=0, maxdd=0, win=0)
    eq = 1.0; peak = 1.0; mdd = 0.0
    for r in rets:
        eq *= (1 + r); peak = max(peak, eq); mdd = min(mdd, eq/peak - 1)
    gains = sum(r for r in rets if r > 0); losses = -sum(r for r in rets if r < 0)
    pf = gains / losses if losses else float('inf')
    mu = st.mean(rets); sd = st.pstdev(rets) or 1e-9
    sharpe = mu / sd * (252 ** 0.5)
    win = sum(1 for r in rets if r > 0) / len(rets)
    return dict(n=len(rets), ret=eq - 1, pf=pf, sharpe=sharpe, maxdd=mdd, win=win)

# ---------- build aligned panel ----------
gold = load_gold()
dates = [r[0] for r in gold]
close = [r[4] for r in gold]
ry = ffill_to(dates, load_fred(DFII10))
dxy = ffill_to(dates, load_fred(DXY))
cot_raw = load_cot_gold()
# lag COT 3 business days for real release timing
cot_lagged = {}
keys = sorted(cot_raw)
for d in keys:
    rel = d + dt.timedelta(days=3)
    cot_lagged[rel] = cot_raw[d]
cot = ffill_to(dates, cot_lagged)

N = len(gold)

def slope_sign(series, t, lb):
    if t - lb < 0 or series[t] is None or series[t - lb] is None: return 0
    diff = series[t] - series[t - lb]
    return -1 if diff > 0 else (1 if diff < 0 else 0)  # FALLING macro driver -> +1 bullish gold

def macro_score(t):
    # real yield weighted 2x (dominant driver), dollar 1x  -> range -3..+3
    return 2 * slope_sign(ry, t, LB_SLOPE) + 1 * slope_sign(dxy, t, LB_SLOPE)

def cot_z(t):
    window = [cot[i] for i in range(max(0, t - 104), t + 1) if cot[i] is not None]
    if len(window) < 30 or cot[t] is None: return 0.0
    mu = st.mean(window); sd = st.pstdev(window) or 1e-9
    return (cot[t] - mu) / sd

def trend_sign(t):
    if t - LB_TREND < 0: return 0
    return 1 if close[t] > close[t - LB_TREND] else -1   # TSMOM

# --- daily emulation of the LIVE RegimeState sustained-bear price gate ---
# RegimeState (H1 EMA200/EMA50, PERSIST=100) ~ on daily: spine EMA~45d, fast EMA~15d,
# spine must be falling over PERSIST_D days, and close<spine and fast<spine = BEAR.
EMA_SPINE_D, EMA_FAST_D, PERSIST_D = 45, 15, 20
_emaS = [None]*N; _emaF = [None]*N
def _build_price_regime():
    kS, kF = 2.0/(EMA_SPINE_D+1), 2.0/(EMA_FAST_D+1); es=ef=None
    for t in range(N):
        c = close[t]
        es = c if es is None else es + kS*(c-es)
        ef = c if ef is None else ef + kF*(c-ef)
        _emaS[t]=es; _emaF[t]=ef
_build_price_regime()
def price_bear(t):  # the EXISTING gate the live gold engines already run
    if t < EMA_SPINE_D + PERSIST_D: return False
    return close[t] < _emaS[t] and _emaS[t] < _emaS[t-PERSIST_D] and _emaF[t] < _emaS[t]

# ---------- strategy variants (long/flat unless noted) ----------
def run(strat):
    rets = []
    for t in range(LB_TREND, N - 1):
        size = strat(t)
        gross = (close[t + 1] / close[t] - 1) * size
        prev = strat(t - 1) if t > LB_TREND else 0.0
        cost = abs(size - prev) * (COST_BPS_RT / 1e4)
        rets.append(gross - cost)
    return rets

def s_buyhold(t):  return 1.0
def s_trend(t):    return 1.0 if trend_sign(t) > 0 else 0.0           # baseline trend long/flat
def s_macro(t):    return 1.0 if macro_score(t) > 0 else 0.0          # macro-only long/flat
def s_veto(t):     # trend, but VETO longs when macro contradicts (<0)
    if trend_sign(t) > 0 and macro_score(t) >= 0: return 1.0
    return 0.0
def s_tilt(t):     # trend long sized by macro alignment (0.5..1.5), floored 0
    if trend_sign(t) <= 0: return 0.0
    return max(0.0, min(1.5, 1.0 + 0.25 * macro_score(t)))
def s_tilt_cot(t): # tilt, but cut size when specs extreme-long (z>1.5) = contrarian fade
    base = s_tilt(t)
    if cot_z(t) > 1.5: base *= 0.5
    return base
def s_protect(t):  # ASYMMETRIC bear-insurance: full trend long normally; de-risk ONLY when
    base = s_trend(t)                 # macro is clearly HOSTILE (real yields rising = score<=-2)
    return 0.0 if macro_score(t) <= -2 else base
def s_protect_short(t):  # same, but flip to half-short when hostile (harvest the bear)
    if macro_score(t) <= -2: return -0.5
    return s_trend(t)
def s_pricegate(t):  # what the live gold book ALREADY does: trend long blocked in price-bear
    return 0.0 if price_bear(t) else s_trend(t)
def s_pricegate_macro(t):  # macro-hostile as a FURTHER tightening ON TOP of the price gate
    if price_bear(t) or macro_score(t) <= -2: return 0.0
    return s_trend(t)
# --- BULL-side mirror of BearProtect (operator Q 2026-06-21): boost size 1.5x ONLY when
#     macro clearly FRIENDLY (real yields falling hard, score>=+2). Mirror of the score<=-2 cut.
def s_boost(t):
    base = s_trend(t)
    return 1.5 * base if macro_score(t) >= 2 else base
def s_protect_boost(t):  # asymmetric BOTH sides: 0 when hostile<=-2, 1.5x when friendly>=+2
    base = s_trend(t)
    if macro_score(t) <= -2: return 0.0
    if macro_score(t) >= 2:  return 1.5 * base
    return base

VARIANTS = [
    ("Buy&Hold (beta)", s_buyhold),
    ("Trend baseline",  s_trend),
    ("Macro-only",      s_macro),
    ("Trend+MacroVeto", s_veto),
    ("Trend+MacroTilt", s_tilt),
    ("Tilt+COTfade",    s_tilt_cot),
    ("Trend+BearProtect", s_protect),
    ("Trend+BullBoost (mirror)", s_boost),
    ("Boost+Protect (both sides)", s_protect_boost),
    ("Protect+BearShort", s_protect_short),
    ("Trend+PriceGate (LIVE today)", s_pricegate),
    ("PriceGate+MacroTighten", s_pricegate_macro),
]

def fmt(m):
    pf = "inf" if m['pf'] == float('inf') else f"{m['pf']:.2f}"
    return f"{m['ret']*100:7.1f}% | PF {pf:>4} | Sh {m['sharpe']:5.2f} | maxDD {m['maxdd']*100:6.1f}% | WR {m['win']*100:4.1f}% | n {m['n']}"

print(f"\nMacroGoldRegime overlay backtest -- gold daily {dates[0]}..{dates[-1]}  ({N} bars)")
print(f"cost {COST_BPS_RT}bps RT | trend LB {LB_TREND}d | macro slope LB {LB_SLOPE}d | COT lagged 3d\n")
half = (N - LB_TREND) // 2
print(f"{'variant':<18}  {'FULL':<70}")
print("-" * 92)
for name, fn in VARIANTS:
    r = run(fn)
    h1, h2 = r[:half], r[half:]
    print(f"{name:<18}  {fmt(metrics(r))}")
    print(f"{'  H1':<18}  {fmt(metrics(h1))}")
    print(f"{'  H2':<18}  {fmt(metrics(h2))}")
    print()
