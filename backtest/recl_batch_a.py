#!/usr/bin/env python3
"""recl_batch_a.py -- Batch A of the thesis-invalidation rollout: does the
failed-breakout (reclaim) exit that validated on XAU Donchian generalise to the
rest of the breakout family (Turtle D1 x4, GoldVolBrk M30, IdxBearShort)?

Generic Donchian-breakout replica, configurable per engine:
  - direction: +1 long-only / -1 short-only / 0 two-way (matches the live engine)
  - optional EMA trend filter (turtles require close>EMA100 for long)
  - ATR(14) Wilder SL=1.5x / TP=3.0x / max_hold bars
  - RECLAIM overlay: exit when a bar CLOSES back through the broken channel level,
    tested in the same grid that won on XAU Donchian (both/short-only x any/<=3 bars)

Per BACKTEST_TRUTH this is bar-replay -> the DELTA (same entries+base exits, only
the overlay differs) is the signal, not absolute PF. Reports per engine whether
ANY reclaim variant is robust (both walk-forward halves positive).

KEY HYPOTHESIS: the XAU winner was SHORT-only (long breakouts that dip back tend
to recover in a bull; short traps are real). So LONG-ONLY turtles likely see the
reclaim HURT, while the SHORT IdxBearShort likely sees it HELP. The point is to
find out per engine, not assume.
"""
import csv, sys
import yfinance as yf

ATR_N = 14
SL_MULT, TP_MULT, MAX_HOLD = 1.5, 3.0, 30
LOCAL_M30 = "/Users/jo/Tick/2yr_XAUUSD_tick_fresh.m30.csv"

# name, source, agg_factor(bars per output bar), N, direction, ema_filter(0=off)
ENGINES = [
    ("XAU_turtle_d1",   "yf:GC=F",  1, 20, +1, 100),
    ("NAS_turtle_d1",   "yf:^NDX",  1, 20, +1, 100),
    ("DJ30_turtle_d1",  "yf:^DJI",  1, 20, +1, 100),
    ("SPX_turtle_d1",   "yf:^GSPC", 1, 20, +1, 100),
    ("GoldVolBrk_m30",  "local",    1, 20, +1,   0),
    ("IdxBearShort_NAS","yf:^NDX",  1, 48, -1,   0),
    ("IdxBearShort_SP", "yf:^GSPC", 1, 48, -1,   0),
]


def load(source):
    """-> list of [ts,o,h,l,c]."""
    if source == "local":
        rows = []
        with open(LOCAL_M30) as f:
            for x in csv.DictReader(f):
                try:
                    rows.append([int(float(x["ts"])), float(x["o"]), float(x["h"]),
                                 float(x["l"]), float(x["c"])])
                except (ValueError, KeyError):
                    continue
        rows.sort(key=lambda z: z[0])
        return rows
    tk = source.split(":", 1)[1]
    d = yf.download(tk, period="5y", interval="1d", progress=False, auto_adjust=False)
    if hasattr(d.columns, "levels"):
        d.columns = [c[0] for c in d.columns]
    d = d.reset_index()
    out = []
    for _, r in d.iterrows():
        try:
            out.append([int(r.iloc[0].value // 10**9), float(r["Open"]), float(r["High"]),
                        float(r["Low"]), float(r["Close"])])
        except (ValueError, KeyError, TypeError):
            continue
    return out


def atr_series(bars):
    out = [None] * len(bars)
    trs, atr, prev_c, ready = [], 0.0, None, False
    for i, (_, o, h, l, c) in enumerate(bars):
        if prev_c is None:
            prev_c = c
        tr = max(h - l, abs(h - prev_c), abs(l - prev_c))
        prev_c = c
        if not ready:
            trs.append(tr)
            if len(trs) == ATR_N:
                atr = sum(trs) / ATR_N; ready = True; out[i] = atr
        else:
            atr = (atr * (ATR_N - 1) + tr) / ATR_N; out[i] = atr
    return out


def ema_series(bars, n):
    if not n:
        return [None] * len(bars)
    out = [None] * len(bars); k = 2.0 / (n + 1); e = None
    for i, b in enumerate(bars):
        c = b[4]
        e = c if e is None else (c * k + e * (1 - k))
        out[i] = e
    return out


def donch(bars, i, n):
    if i < n + 1:
        return None, None
    hi = max(bars[j][2] for j in range(i - n, i))
    lo = min(bars[j][3] for j in range(i - n, i))
    return hi, lo


def simulate(bars, atr, ema, n, direction, reclaim, rsides="both", rmax=0):
    trades, pos = [], None
    for i in range(len(bars)):
        ts, o, h, l, c = bars[i]; a = atr[i]
        if pos:
            held = i - pos["i"]; sl, tp, side = pos["sl"], pos["tp"], pos["side"]
            px = why = None
            if side > 0:
                if l <= sl: px, why = sl, "SL"
                elif h >= tp: px, why = tp, "TP"
            else:
                if h >= sl: px, why = sl, "SL"
                elif l <= tp: px, why = tp, "TP"
            rok = (reclaim and (rmax == 0 or held <= rmax)
                   and (rsides == "both" or side < 0))
            if why is None and rok:
                if side < 0 and c > pos["brk"]: px, why = c, "RECLAIM"
                elif side > 0 and c < pos["brk"]: px, why = c, "RECLAIM"
            if why is None and held >= MAX_HOLD:
                px, why = c, "TIMEOUT"
            if why:
                pnl = (px - pos["e"]) if side > 0 else (pos["e"] - px)
                trades.append((pos["i"], i, side, pnl)); pos = None
        if pos is None and a:
            hi, lo = donch(bars, i, n)
            if hi is not None:
                d = 1 if c > hi else (-1 if c < lo else 0)
                if direction != 0 and d != direction:
                    d = 0
                if d > 0 and ema[i] is not None and c < ema[i]:
                    d = 0   # long trend filter
                if d != 0:
                    pos = {"i": i, "e": c, "side": d,
                           "sl": c - SL_MULT * a if d > 0 else c + SL_MULT * a,
                           "tp": c + TP_MULT * a if d > 0 else c - TP_MULT * a,
                           "brk": lo if d < 0 else hi}
    return trades


def st(trades):
    if not trades:
        return None
    p = [t[3] for t in trades]; w = [x for x in p if x > 0]; ll = [x for x in p if x <= 0]
    pf = sum(w) / abs(sum(ll)) if ll and sum(ll) != 0 else 99.0
    return dict(n=len(trades), win=100 * len(w) / len(trades), net=sum(p), pf=pf)


def run_engine(name, source, agg, n, direction, ema_n):
    bars = load(source)
    if len(bars) < n + ATR_N + 5:
        print(f"{name:<18} (insufficient data: {len(bars)} bars)"); return
    atr = atr_series(bars); ema = ema_series(bars, ema_n); mid = len(bars) // 2
    base = simulate(bars, atr, ema, n, direction, reclaim=False)
    sb = st(base)
    dtag = {1: "LONG-only", -1: "SHORT-only", 0: "two-way"}[direction]
    print(f"\n===== {name}  ({dtag}, DonchN{n}{', EMA'+str(ema_n) if ema_n else ''}) =====")
    if not sb:
        print("  baseline: no trades"); return
    print(f"  baseline           n={sb['n']:>4} win={sb['win']:5.1f}% net={sb['net']:>+9.1f} PF={sb['pf']:5.2f}")
    variants = [("both/any", dict(reclaim=True)),
                ("both/<=3b", dict(reclaim=True, rmax=3)),
                ("short/any", dict(reclaim=True, rsides="short")),
                ("short/<=3b", dict(reclaim=True, rsides="short", rmax=3))]
    best = None
    for vn, kw in variants:
        var = simulate(bars, atr, ema, n, direction, **kw)
        sv = st(var)
        if not sv:
            continue
        d = sv["net"] - sb["net"]
        b1 = st([t for t in base if t[0] < mid]); v1 = st([t for t in var if t[0] < mid])
        b2 = st([t for t in base if t[0] >= mid]); v2 = st([t for t in var if t[0] >= mid])
        d1 = (v1["net"] - b1["net"]) if (b1 and v1) else 0
        d2 = (v2["net"] - b2["net"]) if (b2 and v2) else 0
        robust = d1 > 0 and d2 > 0 and d > 0
        flag = "ROBUST(both+)" if robust else "not-robust"
        print(f"   {vn:<11} Δ={d:>+8.1f}  H1Δ={d1:>+7.1f} H2Δ={d2:>+7.1f}  PF {sb['pf']:.2f}->{sv['pf']:.2f}  {flag}")
        if robust and (best is None or d > best[1]):
            best = (vn, d)
    print(f"  >>> {name}: {'SHIP '+best[0]+f' (+{best[1]:.0f})' if best else 'NO robust reclaim variant -- leave as-is'}")


def main():
    print("BATCH A -- failed-breakout (reclaim) exit across the breakout family")
    for e in ENGINES:
        try:
            run_engine(*e)
        except Exception as ex:
            print(f"\n{e[0]:<18} ERROR: {type(ex).__name__}: {ex}")


if __name__ == "__main__":
    main()
