#!/usr/bin/env python3
"""losscut_batch_b.py -- Batch B: the trend-follower in-flight protection is the
EXISTING LOSS_CUT_PCT knob (cold-loss cut, runs before SL/TP), currently 0.0
(OFF) on the XauTrendFollow cluster. Does turning it on protect from a big
adverse swing WITHOUT killing the trend edge?

Sweeps LOSS_CUT_PCT on long-only Donchian/EMA100 trend-followers (the turtle
family = a faithful trend-follower proxy: Donch20 breakout + EMA100 filter,
long-only). LOSS_CUT cuts when adverse >= entry * pct/100, before the 1.5xATR SL.

Memory ([[omega-runner-profit-protect-regime]]): aggressive giveback/tight cuts
HURT runners. Hypothesis: tightening LOSS_CUT bleeds the trend edge; only a WIDE
cut (catching the catastrophic cold-loss tail) might be neutral-to-positive.
The point is to verify per the data, not assume.
"""
import csv
import yfinance as yf

ATR_N = 14
SL_MULT, TP_MULT, MAX_HOLD = 1.5, 3.0, 30
ENGINES = [  # name, ticker, N
    ("XAU_tf", "GC=F", 20), ("NAS_tf", "^NDX", 20),
    ("SPX_tf", "^GSPC", 20), ("DJ30_tf", "^DJI", 20),
]
CUTS = [0.0, 0.5, 1.0, 1.5, 2.0]  # LOSS_CUT_PCT (% of entry)


def load(tk):
    d = yf.download(tk, period="5y", interval="1d", progress=False, auto_adjust=False)
    if hasattr(d.columns, "levels"):
        d.columns = [c[0] for c in d.columns]
    d = d.reset_index()
    out = []
    for _, r in d.iterrows():
        try:
            out.append([float(r["Open"]), float(r["High"]), float(r["Low"]), float(r["Close"])])
        except (ValueError, KeyError, TypeError):
            continue
    return out


def atr_series(bars):
    out = [None] * len(bars); trs = []; atr = 0.0; prev = None; ready = False
    for i, (o, h, l, c) in enumerate(bars):
        if prev is None:
            prev = c
        tr = max(h - l, abs(h - prev), abs(l - prev)); prev = c
        if not ready:
            trs.append(tr)
            if len(trs) == ATR_N:
                atr = sum(trs) / ATR_N; ready = True; out[i] = atr
        else:
            atr = (atr * (ATR_N - 1) + tr) / ATR_N; out[i] = atr
    return out


def ema_series(bars, n):
    out = [None] * len(bars); k = 2.0 / (n + 1); e = None
    for i, b in enumerate(bars):
        e = b[3] if e is None else (b[3] * k + e * (1 - k)); out[i] = e
    return out


def simulate(bars, atr, ema, n, loss_cut_pct):
    trades, pos = [], None
    for i in range(len(bars)):
        o, h, l, c = bars[i]; a = atr[i]
        if pos:
            held = i - pos["i"]; e = pos["e"]
            adverse = e - l  # long-only
            px = why = None
            if loss_cut_pct > 0 and adverse >= e * loss_cut_pct / 100.0:
                px, why = e - e * loss_cut_pct / 100.0, "LOSS_CUT"   # cut at the threshold
            elif l <= pos["sl"]:
                px, why = pos["sl"], "SL"
            elif h >= pos["tp"]:
                px, why = pos["tp"], "TP"
            elif held >= MAX_HOLD:
                px, why = c, "TIMEOUT"
            if why:
                trades.append((pos["i"], px - e)); pos = None
        if pos is None and a:
            if i >= n + 1:
                hi = max(bars[j][1] for j in range(i - n, i))
                if c > hi and ema[i] is not None and c > ema[i]:  # long breakout + trend filter
                    pos = {"i": i, "e": c, "sl": c - SL_MULT * a, "tp": c + TP_MULT * a}
    return trades


def stat(trades):
    if not trades:
        return None
    p = [t[1] for t in trades]; w = [x for x in p if x > 0]; ll = [x for x in p if x <= 0]
    pf = sum(w) / abs(sum(ll)) if ll and sum(ll) != 0 else 99.0
    mdd = 0; cum = 0; peak = 0
    for x in p:
        cum += x; peak = max(peak, cum); mdd = min(mdd, cum - peak)
    return dict(n=len(p), net=sum(p), pf=pf, mdd=mdd, worst=min(p))


def main():
    print("BATCH B -- LOSS_CUT_PCT sweep on the long-only trend-follower cluster\n")
    for name, tk, n in ENGINES:
        bars = load(tk); atr = atr_series(bars); ema = ema_series(bars, 100); mid = len(bars) // 2
        base = simulate(bars, atr, ema, n, 0.0); sb = stat(base)
        if not sb:
            print(f"{name}: no trades"); continue
        print(f"===== {name} (Donch{n}/EMA100 long) =====")
        for cut in CUTS:
            tr = simulate(bars, atr, ema, n, cut); s = stat(tr)
            d = s["net"] - sb["net"]
            h1 = stat([t for t in tr if t[0] < mid]); b1 = stat([t for t in base if t[0] < mid])
            h2 = stat([t for t in tr if t[0] >= mid]); b2 = stat([t for t in base if t[0] >= mid])
            d1 = (h1["net"] - b1["net"]) if (h1 and b1) else 0
            d2 = (h2["net"] - b2["net"]) if (h2 and b2) else 0
            tag = "" if cut == 0 else ("  ROBUST+" if (d > 0 and d1 > 0 and d2 > 0) else
                                       ("  worse" if d < 0 else "  mixed"))
            print(f"   LC={cut:>3}%  net={s['net']:>+9.1f} Δ={d:>+8.1f}  PF={s['pf']:4.2f} "
                  f"maxDD={s['mdd']:>+8.1f} worst={s['worst']:>+7.1f}{tag}")
        print()


if __name__ == "__main__":
    main()
