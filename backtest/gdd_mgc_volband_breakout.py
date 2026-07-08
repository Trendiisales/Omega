#!/usr/bin/env python3
"""gdd_mgc_volband_breakout.py -- Study 7: Donchian breakout x vol-regime band on MGC 30m/H1.

Entry: close-confirmed Donchian N-channel break -> enter next bar OPEN (no intrabar entry).
Exit:  hard stop = entry -/+ SLx*ATR14 (intrabar, ADVERSE-FIRST: stop checked before any
       favorable exit within the same bar) OR opposite Nout-channel mid close-exit.
Vol band: ATR14 percentile within trailing 500 bars; bands all / low(<30) / mid(30-70) / high(>70).
Cost 0.31pt RT MGC (2x stress 0.62). Both directions. Data certified 2024-06..2026-06.
2022 shadow: same rules on XAU M30 aggregated from xau_m1_2022bear.csv at MGC-cost proxy.
"""
import sys, bisect
sys.path.insert(0, "/Users/jo/Omega/backtest")
from gdd_lib import load_bars, atr_series, perf, fmt

def donchian_bt(bars, N, Nout, sl_mult, side, band, cost):
    atr = atr_series(bars, 14)
    # rolling ATR percentile
    hist = []
    tr = []
    pos = None
    hi = [b["h"] for b in bars]; lo = [b["l"] for b in bars]
    for i in range(len(bars)):
        b = bars[i]
        a = atr[i]
        if a is None: continue
        # percentile of current atr in trailing 500
        if len(hist) >= 100:
            srt = sorted(hist[-500:])
            pct = bisect.bisect_left(srt, a) / len(srt) * 100
        else:
            pct = 50
        hist.append(a)
        if pos:
            # adverse-first intrabar stop
            if pos["side"] == 1:
                if b["l"] <= pos["stop"]:
                    px = min(b["o"], pos["stop"])
                    tr.append(dict(entry_ts=pos["ts"], pnl=(px - pos["e"]) - cost)); pos = None
                elif i - 1 >= Nout and b["c"] < min(lo[i - Nout:i]):
                    tr.append(dict(entry_ts=pos["ts"], pnl=(b["c"] - pos["e"]) - cost)); pos = None
            else:
                if b["h"] >= pos["stop"]:
                    px = max(b["o"], pos["stop"])
                    tr.append(dict(entry_ts=pos["ts"], pnl=(pos["e"] - px) - cost)); pos = None
                elif i - 1 >= Nout and b["c"] > max(hi[i - Nout:i]):
                    tr.append(dict(entry_ts=pos["ts"], pnl=(pos["e"] - b["c"]) - cost)); pos = None
        if pos is None and i >= N and i + 1 < len(bars):
            in_band = (band == "all" or (band == "low" and pct < 30)
                       or (band == "mid" and 30 <= pct <= 70) or (band == "high" and pct > 70))
            if not in_band: continue
            if side == 1 and b["c"] > max(hi[i - N:i]):
                e = bars[i + 1]["o"]
                pos = dict(side=1, e=e, stop=e - sl_mult * a, ts=bars[i + 1]["ts"])
            elif side == -1 and b["c"] < min(lo[i - N:i]):
                e = bars[i + 1]["o"]
                pos = dict(side=-1, e=e, stop=e + sl_mult * a, ts=bars[i + 1]["ts"])
    return tr

if __name__ == "__main__":
    path = sys.argv[1] if len(sys.argv) > 1 else "/Users/jo/Tick/mgc_30m_hist.csv"
    cost = float(sys.argv[2]) if len(sys.argv) > 2 else 0.31
    tag = sys.argv[3] if len(sys.argv) > 3 else "MGC30m"
    bars = load_bars(path)
    print(f"# {tag} n={len(bars)} cost={cost}")
    for side, sn in ((1, "L"), (-1, "S")):
        for N in (20, 40, 55):
            for band in ("all", "low", "mid", "high"):
                m = perf(donchian_bt(bars, N, max(10, N // 2), 3.0, side, band, cost),
                         f"{tag} Don{N} {band} {sn} sl3.0")
                if m["n"] >= 20: print(fmt(m))
