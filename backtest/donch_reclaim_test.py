#!/usr/bin/env python3
"""donch_reclaim_test.py -- does a FAILED-BREAKOUT (reclaim) exit improve the
SurvivorPortfolio XAU 4h Donchian cells, or does it bleed the trades that recover?

Engine-faithful replica of the cell (include/SurvivorPortfolio.hpp):
  - ATR(14) Wilder: seed = mean(first 14 TR), then atr=(atr*13+tr)/14;
    TR = max(h-l, |h-prevclose|, |l-prevclose|)
  - Donchian channel over the prior N bars (EXCLUDING current); signal on CLOSE:
    close > hi -> LONG (+1), close < lo -> SHORT (-1)
  - entry at bar close; SL = 1.5*ATR, TP = 3*ATR, max_hold = 30 bars
  - SL/TP detected intrabar via bar high/low (SL checked first = conservative)

RECLAIM overlay (the change under test): a SHORT exits the moment a later bar
CLOSES back ABOVE the channel-low it broke (d.lo at entry); a LONG exits when a
bar closes back BELOW the channel-high it broke. Checked AFTER SL/TP, so it only
helps when price snaps back without first hitting the full stop -- the bear trap.

Reports baseline vs +reclaim: n, win%, net pts, PF, both walk-forward halves,
and long/short split. Per BACKTEST_TRUTH this is bar-replay -> PF is a relative
hint; the SIGNAL is the DELTA (same entries, same base exits, only the overlay
differs).
"""
import csv, sys
from statistics import median

M30 = "/Users/jo/Tick/2yr_XAUUSD_tick_fresh.m30.csv"
ATR_N = 14
SL_MULT, TP_MULT, MAX_HOLD = 1.5, 3.0, 30


def load_h4(path):
    """Aggregate M30 (ts,o,h,l,c) -> H4 bars (8 x 30min = 240min buckets)."""
    rows = []
    with open(path) as f:
        r = csv.DictReader(f)
        for x in r:
            try:
                rows.append((int(float(x["ts"])), float(x["o"]), float(x["h"]),
                             float(x["l"]), float(x["c"])))
            except (ValueError, KeyError):
                continue
    rows.sort(key=lambda z: z[0])
    bars, cur, bucket = [], None, None
    H4 = 4 * 3600
    for ts, o, h, l, c in rows:
        b = ts // H4
        if b != bucket:
            if cur:
                bars.append(cur)
            cur = [ts, o, h, l, c]
            bucket = b
        else:
            cur[2] = max(cur[2], h); cur[3] = min(cur[3], l); cur[4] = c
    if cur:
        bars.append(cur)
    return bars  # [ts,o,h,l,c]


def atr_series(bars):
    """Wilder ATR(14), faithful to push_bar_internal. atr[i] valid from i>=14."""
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
                atr = sum(trs) / ATR_N
                ready = True
                out[i] = atr
        else:
            atr = (atr * (ATR_N - 1) + tr) / ATR_N
            out[i] = atr
    return out


def donch(bars, i, n):
    """Channel hi/lo over the n bars BEFORE i (faithful: excludes current)."""
    if i < n + 1:
        return None, None
    hi = max(bars[j][2] for j in range(i - n, i))
    lo = min(bars[j][3] for j in range(i - n, i))
    return hi, lo


def simulate(bars, atr, n, reclaim, reclaim_sides="both", reclaim_max_bars=0):
    """reclaim_sides: 'both'|'short' (only cut short traps). reclaim_max_bars:
    0 = any time; K = only fire the reclaim within K bars of entry (fast trap)."""
    trades = []
    pos = None  # dict
    for i in range(len(bars)):
        ts, o, h, l, c = bars[i]
        a = atr[i]
        # ---- manage open position FIRST (intrabar SL/TP, then reclaim on close) ----
        if pos:
            held = i - pos["entry_i"]
            sl, tp, side = pos["sl"], pos["tp"], pos["side"]
            exit_px = why = None
            if side > 0:
                if l <= sl: exit_px, why = sl, "SL"
                elif h >= tp: exit_px, why = tp, "TP"
            else:
                if h >= sl: exit_px, why = sl, "SL"
                elif l <= tp: exit_px, why = tp, "TP"
            reclaim_ok = (reclaim and (reclaim_max_bars == 0 or held <= reclaim_max_bars)
                          and (reclaim_sides == "both" or side < 0))
            if why is None and reclaim_ok:
                # failed breakout: close back through the broken channel level
                if side < 0 and c > pos["break_lvl"]:
                    exit_px, why = c, "RECLAIM"
                elif side > 0 and c < pos["break_lvl"]:
                    exit_px, why = c, "RECLAIM"
            if why is None and held >= MAX_HOLD:
                exit_px, why = c, "TIMEOUT"
            if why:
                pnl = (exit_px - pos["entry"]) if side > 0 else (pos["entry"] - exit_px)
                trades.append((pos["entry_i"], i, side, pnl, why))
                pos = None
        # ---- entry (only when flat) ----
        if pos is None and a:
            hi, lo = donch(bars, i, n)
            if hi is not None:
                d = 1 if c > hi else (-1 if c < lo else 0)
                if d != 0:
                    pos = {"entry_i": i, "entry": c, "side": d,
                           "sl": c - SL_MULT * a if d > 0 else c + SL_MULT * a,
                           "tp": c + TP_MULT * a if d > 0 else c - TP_MULT * a,
                           "break_lvl": lo if d < 0 else hi}
    return trades


def stats(trades):
    if not trades:
        return None
    pnls = [t[3] for t in trades]
    wins = [p for p in pnls if p > 0]
    losses = [p for p in pnls if p <= 0]
    pf = sum(wins) / abs(sum(losses)) if losses and sum(losses) != 0 else 99.0
    return dict(n=len(trades), win=100 * len(wins) / len(trades), net=sum(pnls), pf=pf,
                avg_w=sum(wins) / len(wins) if wins else 0,
                avg_l=sum(losses) / len(losses) if losses else 0)


def show(tag, trades):
    s = stats(trades)
    if not s:
        print(f"{tag:<26} (no trades)"); return
    print(f"{tag:<26} n={s['n']:>4} win={s['win']:5.1f}% net={s['net']:>+9.1f}pt "
          f"PF={s['pf']:5.2f} avgW={s['avg_w']:+.1f} avgL={s['avg_l']:+.1f}")


def main():
    bars = load_h4(M30)
    atr = atr_series(bars)
    mid_i = len(bars) // 2
    print(f"H4 bars: {len(bars)}  ({bars[0][0]} -> {bars[-1][0]})  WF split @ bar {mid_i}\n")

    def wf(tag, base, var):
        """Print net + both-WF-halves deltas; flag robust only if BOTH halves +."""
        b, v = stats(base), stats(var)
        d = v["net"] - b["net"]
        b1 = stats([t for t in base if t[0] < mid_i]); v1 = stats([t for t in var if t[0] < mid_i])
        b2 = stats([t for t in base if t[0] >= mid_i]); v2 = stats([t for t in var if t[0] >= mid_i])
        d1 = (v1["net"] - b1["net"]) if (b1 and v1) else 0
        d2 = (v2["net"] - b2["net"]) if (b2 and v2) else 0
        robust = d1 > 0 and d2 > 0
        print(f"   {tag:<22} net {b['net']:>+8.0f} -> {v['net']:>+8.0f}  "
              f"Δ={d:>+7.0f}  | H1Δ={d1:>+7.0f} H2Δ={d2:>+7.0f}  "
              f"PF {b['pf']:.2f}->{v['pf']:.2f}  {'ROBUST(both+)' if robust else 'not-robust'}")

    for n in (20, 100):
        print(f"===== XAU 4h DonchN{n} =====")
        base = simulate(bars, atr, n, reclaim=False)
        wf("reclaim both/any",  base, simulate(bars, atr, n, True))
        wf("reclaim both/<=3b", base, simulate(bars, atr, n, True, reclaim_max_bars=3))
        wf("reclaim both/<=5b", base, simulate(bars, atr, n, True, reclaim_max_bars=5))
        wf("reclaim SHORT/any", base, simulate(bars, atr, n, True, reclaim_sides="short"))
        wf("reclaim SHORT/<=3b",base, simulate(bars, atr, n, True, reclaim_sides="short", reclaim_max_bars=3))
        print()


if __name__ == "__main__":
    main()
