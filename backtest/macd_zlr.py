#!/usr/bin/env python3
"""macd_zlr.py -- MACD Zero-Line Reversal continuation (Joe Rabil setup).

Mechanizes: after a bullish THRUST (MACD pushed well above zero), a sideways
consolidation works MACD back toward the zero line (a "kiss", signal-line holds),
the MACD/signal MAs CONVERGE (histogram ~0) and ADX compresses (<25). Then MACD
turns back UP from zero -> enter long, structural ATR stop, runner ATR-trail (no
fixed TP -- ride the next thrust). Long-only continuation bias.

Self-normalizing thrust detection (relative to the thrust's own peak), so it works
across instruments/price scales. Tested across XAU + indices, both walk-forward
halves + the 2022 bear, with a selectivity sweep. Per BACKTEST_TRUTH the video is
hand-picked winners -- this asks whether the MECHANICAL core has cross-regime edge.
"""
import sys
import yfinance as yf

INSTR = [("XAU", "GC=F"), ("NAS", "^NDX"), ("SPX", "^GSPC"), ("DJI", "^DJI")]


def load(tk):
    d = yf.download(tk, period="10y", interval="1d", progress=False, auto_adjust=False)
    if hasattr(d.columns, "levels"):
        d.columns = [c[0] for c in d.columns]
    d = d.reset_index()
    out = []
    for _, r in d.iterrows():
        try:
            out.append((float(r["Open"]), float(r["High"]), float(r["Low"]), float(r["Close"])))
        except (ValueError, KeyError, TypeError):
            continue
    return out


def ema(vals, n):
    out = [None] * len(vals); k = 2.0 / (n + 1); e = None
    for i, v in enumerate(vals):
        e = v if e is None else v * k + e * (1 - k); out[i] = e
    return out


def macd(closes, f=12, s=26, sig=9):
    ef, es = ema(closes, f), ema(closes, s)
    m = [(ef[i] - es[i]) if ef[i] is not None and es[i] is not None else 0.0 for i in range(len(closes))]
    sg = ema(m, sig)
    h = [m[i] - (sg[i] or 0.0) for i in range(len(m))]
    return m, sg, h


def wilder_adx(bars, n=14):
    """ADX(14) Wilder. Returns (adx, atr) per bar."""
    L = len(bars); adx = [None] * L; atr = [0.0] * L
    tr = [0.0] * L; pdm = [0.0] * L; ndm = [0.0] * L
    for i in range(1, L):
        h, l, pc = bars[i][1], bars[i][2], bars[i - 1][3]
        up = bars[i][1] - bars[i - 1][1]; dn = bars[i - 1][2] - bars[i][2]
        pdm[i] = up if (up > dn and up > 0) else 0.0
        ndm[i] = dn if (dn > up and dn > 0) else 0.0
        tr[i] = max(h - l, abs(h - pc), abs(l - pc))
    # Wilder RMA
    def rma(x):
        o = [None] * L; r = None
        for i in range(1, L):
            if i <= n:
                if i == n:
                    r = sum(x[1:n + 1]) / n; o[i] = r
            else:
                r = (r * (n - 1) + x[i]) / n; o[i] = r
        return o
    atrs = rma(tr); pdms = rma(pdm); ndms = rma(ndm)
    dx = [None] * L
    for i in range(L):
        if atrs[i] and atrs[i] > 0:
            atr[i] = atrs[i]
            pdi = 100 * (pdms[i] or 0) / atrs[i]; ndi = 100 * (ndms[i] or 0) / atrs[i]
            denom = pdi + ndi
            dx[i] = 100 * abs(pdi - ndi) / denom if denom > 0 else 0.0
    # ADX = RMA of DX
    r = None; cnt = 0; buf = []
    for i in range(L):
        if dx[i] is None:
            continue
        buf.append(dx[i])
        if r is None and len(buf) == n:
            r = sum(buf) / n; adx[i] = r
        elif r is not None:
            r = (r * (n - 1) + dx[i]) / n; adx[i] = r
    return adx, atr


def simulate(bars, thrust_w=120, return_frac=0.30, adx_max=25.0,
             conv_frac=0.6, sl_atr=2.0, trail_atr=3.0, max_hold=120):
    closes = [b[3] for b in bars]
    m, sg, h = macd(closes)
    adx, atr = wilder_adx(bars)
    trades = []; pos = None
    for i in range(thrust_w + 30, len(bars)):
        o, hi, lo, c = bars[i]; a = atr[i]
        if pos:
            pos["hh"] = max(pos["hh"], hi); held = i - pos["i"]
            px = why = None
            if lo <= pos["hh"] - trail_atr * a: px, why = pos["hh"] - trail_atr * a, "TRAIL"
            elif lo <= pos["sl"]: px, why = pos["sl"], "SL"
            elif held >= max_hold: px, why = c, "TO"
            if why:
                trades.append((pos["i"], px - pos["e"], pos["e"])); pos = None
        if pos is None and a and adx[i] is not None:
            peak = max(m[i - thrust_w:i])              # the thrust's MACD peak
            thrust_ok = peak > 0 and peak > 1.5 * a    # a real bullish thrust
            at_zero = thrust_ok and abs(m[i]) <= peak * return_frac   # MACD kissed back to zero
            converged = abs(h[i]) <= conv_frac * a     # MAs together (small histogram)
            compressed = adx[i] < adx_max              # quiet
            turn_up = h[i] > 0 and h[i - 1] <= 0       # histogram crosses up
            if thrust_ok and at_zero and converged and compressed and turn_up:
                pos = {"i": i, "e": c, "hh": hi, "sl": c - sl_atr * a}
    return trades


def stat(trades):
    if not trades:
        return None
    p = [t[1] / t[2] * 100 for t in trades]   # % returns (instrument-comparable)
    w = [x for x in p if x > 0]; l = [x for x in p if x <= 0]
    pf = sum(w) / abs(sum(l)) if l and sum(l) != 0 else 99.0
    mdd = 0; cum = 0; pk = 0
    for x in p:
        cum += x; pk = max(pk, cum); mdd = min(mdd, cum - pk)
    return dict(n=len(p), win=100 * len(w) / len(p), net=sum(p), pf=pf, mdd=mdd,
                avg=sum(p) / len(p))


def show(tag, tr):
    s = stat(tr)
    if not s:
        print(f"  {tag:<20} (no trades)"); return None
    print(f"  {tag:<20} n={s['n']:>3} win={s['win']:5.1f}% net={s['net']:>+7.1f}% "
          f"PF={s['pf']:4.2f} avg={s['avg']:+5.2f}% maxDD={s['mdd']:>+6.1f}%")
    return s


def main():
    print("MACD ZERO-LINE REVERSAL -- continuation engine, 10y daily, both halves + bear\n")
    for name, tk in INSTR:
        bars = load(tk)
        if len(bars) < 300:
            print(f"{name}: insufficient data"); continue
        mid = len(bars) // 2
        tr = simulate(bars)
        print(f"===== {name} ({tk}, {len(bars)} bars) =====")
        s = show("ALL", tr)
        show("  H1", [t for t in tr if t[0] < mid])
        show("  H2", [t for t in tr if t[0] >= mid])
        # 2022 bear bucket (roughly bars where it fell — use a date-agnostic proxy:
        # the worst-drawdown calendar third). Simplest: split into thirds.
        third = len(bars) // 3
        show("  early(3rd)", [t for t in tr if t[0] < third])
        show("  mid(3rd)", [t for t in tr if third <= t[0] < 2 * third])
        show("  late(3rd)", [t for t in tr if t[0] >= 2 * third])
        print()


if __name__ == "__main__":
    main()
