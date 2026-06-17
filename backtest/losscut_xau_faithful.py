#!/usr/bin/env python3
"""losscut_xau_faithful.py -- (a) faithful per-TF LOSS_CUT test on the LIVE
xau_tf cluster (4h/2h/1h), the engines whose LOSS_CUT_PCT is 0.0 today.

Uses the local 2yr XAU M30 tape (real intraday, not a daily proxy), aggregated
to each cell TF, with a two-way trend-follower signal (Donchian-N breakout long
+ short — the xau_tf cells are momentum/trend, two-way). Sweeps LOSS_CUT_PCT to
find a value that caps the big adverse swing WITHOUT killing the trend edge.
Reports net / maxDD / worst-trade / both-WF-halves per TF.
"""
import csv

M30 = "/Users/jo/Tick/2yr_XAUUSD_tick_fresh.m30.csv"
ATR_N = 14
SL_MULT, TP_MULT, MAX_HOLD = 1.5, 3.0, 30
CUTS = [0.0, 0.5, 0.75, 1.0, 1.5, 2.0]
TFS = [("H1", 2, 20), ("H2", 4, 20), ("H4", 8, 20)]  # name, m30-per-bar, Donch N


def load_m30():
    rows = []
    with open(M30) as f:
        for x in csv.DictReader(f):
            try:
                rows.append([float(x["o"]), float(x["h"]), float(x["l"]), float(x["c"])])
            except (ValueError, KeyError):
                continue
    return rows


def agg(m30, k):
    out = []
    for i in range(0, len(m30) - k + 1, k):
        chunk = m30[i:i + k]
        out.append([chunk[0][0], max(c[1] for c in chunk), min(c[2] for c in chunk), chunk[-1][3]])
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


def simulate(bars, atr, n, loss_cut_pct):
    trades, pos = [], None
    for i in range(len(bars)):
        o, h, l, c = bars[i]; a = atr[i]
        if pos:
            held = i - pos["i"]; e = pos["e"]; side = pos["side"]
            adverse = (e - l) if side > 0 else (h - e)
            px = why = None
            if loss_cut_pct > 0 and adverse >= e * loss_cut_pct / 100.0:
                cut = e * loss_cut_pct / 100.0
                px, why = (e - cut) if side > 0 else (e + cut), "LOSS_CUT"
            elif side > 0:
                if l <= pos["sl"]: px, why = pos["sl"], "SL"
                elif h >= pos["tp"]: px, why = pos["tp"], "TP"
            else:
                if h >= pos["sl"]: px, why = pos["sl"], "SL"
                elif l <= pos["tp"]: px, why = pos["tp"], "TP"
            if why is None and held >= MAX_HOLD:
                px, why = c, "TIMEOUT"
            if why:
                trades.append((pos["i"], (px - e) if side > 0 else (e - px))); pos = None
        if pos is None and a and i >= n + 1:
            hi = max(bars[j][1] for j in range(i - n, i))
            lo = min(bars[j][2] for j in range(i - n, i))
            d = 1 if c > hi else (-1 if c < lo else 0)
            if d != 0:
                pos = {"i": i, "e": c, "side": d,
                       "sl": c - SL_MULT * a if d > 0 else c + SL_MULT * a,
                       "tp": c + TP_MULT * a if d > 0 else c - TP_MULT * a}
    return trades


def stat(tr):
    if not tr:
        return None
    p = [t[1] for t in tr]; w = [x for x in p if x > 0]; ll = [x for x in p if x <= 0]
    pf = sum(w) / abs(sum(ll)) if ll and sum(ll) != 0 else 99.0
    mdd = 0; cum = 0; peak = 0
    for x in p:
        cum += x; peak = max(peak, cum); mdd = min(mdd, cum - peak)
    return dict(n=len(p), net=sum(p), pf=pf, mdd=mdd, worst=min(p))


def main():
    print("(a) FAITHFUL XAU LOSS_CUT -- live xau_tf cluster, real intraday M30->TF\n")
    m30 = load_m30()
    for tfname, k, n in TFS:
        bars = agg(m30, k); atr = atr_series(bars); mid = len(bars) // 2
        base = simulate(bars, atr, n, 0.0); sb = stat(base)
        print(f"===== XAU {tfname} (Donch{n} two-way, n={sb['n']}) =====")
        for cut in CUTS:
            tr = simulate(bars, atr, n, cut); s = stat(tr)
            d = s["net"] - sb["net"]
            h1 = stat([t for t in tr if t[0] < mid]); b1 = stat([t for t in base if t[0] < mid])
            h2 = stat([t for t in tr if t[0] >= mid]); b2 = stat([t for t in base if t[0] >= mid])
            d1 = (h1["net"] - b1["net"]) if (h1 and b1) else 0
            d2 = (h2["net"] - b2["net"]) if (h2 and b2) else 0
            ddpct = 100 * (s["mdd"] - sb["mdd"]) / abs(sb["mdd"]) if sb["mdd"] else 0
            tag = "" if cut == 0 else (
                "  WIN(net+ DD-)" if (d >= -abs(sb['net'])*0.05 and s['mdd'] > sb['mdd']) else
                ("  worse" if d < 0 and s['mdd'] <= sb['mdd'] else "  mixed"))
            print(f"   LC={cut:>4}%  net={s['net']:>+8.1f} Δ={d:>+7.1f}  PF={s['pf']:4.2f} "
                  f"maxDD={s['mdd']:>+8.1f}({ddpct:>+5.0f}%) worst={s['worst']:>+6.1f}  "
                  f"H1Δ={d1:>+6.0f} H2Δ={d2:>+6.0f}{tag}")
        print()


if __name__ == "__main__":
    main()
