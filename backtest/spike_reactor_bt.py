#!/usr/bin/env python3
# =============================================================================
# spike_reactor_bt.py -- faithful backtest of a cross-EQUITY SPIKE REACTOR.
# Operator 2026-06-22: "an overarching engine that reacts when the system detects
# a spike in price; find the optimum window (maybe less, maybe 20 min)."
#
# SCOPE: big-cap equity 15m universe ONLY (the asset class where spike-reaction
# survives cost -- gold/index spot is structurally dead, see
# [[omega-intraday-spot-cfd-cost-wall]]). Generalizes the validated BigCapMomo.
#
# MECHANIC: each bar, measure the W-bar move normalized by ATR14. If
# move >= k*ATR -> SPIKE UP -> go long next bar open (continuation). move <= -k*ATR
# -> SPIKE DOWN -> go short. Exit = wide ATR trail (BigCapMomo: ride, don't tighten)
# OR hard stop OR end-of-day flat (intraday reactor, no overnight gap risk).
#
# FAITHFUL: entry at NEXT bar OPEN (no look-ahead); realistic equity cost in bps
# (entry slip 3 + exit slip 2 + commission 1/side = ~7bps round trip on a chase);
# SL-first intrabar; per-name cooldown; PnL in bps of notional, equal-weight.
#
# SWEEP: window W in {1,2,3,4} bars (15/30/45/60 min) x threshold k in {1.5..3.0}.
# Reports n / WR / PF / net-bps / maxDD + both-WF-halves (H1/H2 PF). BACKTEST_TRUTH:
# only both-halves+ is interesting; this data is 2024-06..2026-06 = BULL ONLY
# (no 2022 bear in 15m stock data -- same caveat as BigCapMomo).
#
# RUN: python3 spike_reactor_bt.py /tmp/stocks15m
# =============================================================================
import sys, os, glob, csv
from collections import defaultdict

ENTRY_SLIP_BPS = 3.0    # adverse fill chasing the spike
EXIT_SLIP_BPS  = 2.0
COMMISSION_BPS = 1.0    # per side
COST_RT_BPS    = ENTRY_SLIP_BPS + EXIT_SLIP_BPS + 2 * COMMISSION_BPS  # ~7 bps round trip
TRAIL_ATR      = 3.0    # wide trail (BigCapMomo lesson: ride)
STOP_ATR       = 2.0    # hard adverse stop
MAX_HOLD       = 13     # bars (~3.25h); also force-flat at end of day
COOLDOWN       = 4      # bars after a trade per name
ATR_N          = 14

def load(path):
    ts, o, h, l, c = [], [], [], [], []
    with open(path, newline="") as fh:
        r = csv.reader(fh); next(r, None)
        for row in r:
            if len(row) < 5: continue
            try:
                ts.append(int(float(row[0]))); o.append(float(row[1])); h.append(float(row[2]))
                l.append(float(row[3])); c.append(float(row[4]))
            except ValueError: continue
    return ts, o, h, l, c

def atr14(o, h, l, c):
    n = len(c); atr = [0.0] * n; tr_sum = 0.0
    for i in range(1, n):
        tr = max(h[i] - l[i], abs(h[i] - c[i-1]), abs(l[i] - c[i-1]))
        if i <= ATR_N: tr_sum += tr; atr[i] = tr_sum / i
        else: atr[i] = (atr[i-1] * (ATR_N - 1) + tr) / ATR_N
    return atr

def day_of(t): return t // 86400

def run_name(path, W, k, mode=1, fade_tp=None, max_hold=MAX_HOLD, eod_flat=True):
    """Trades for one name. mode=+1 continuation (chase), -1 fade (counter).
    fade_tp (ATR mult) gives a mean-reversion take-profit instead of the wide trail.
    eod_flat=False + large max_hold = BigCapMomo-style multi-day swing hold."""
    ts, o, h, l, c = load(path)
    n = len(c)
    if n < ATR_N + W + 5: return []
    atr = atr14(o, h, l, c)
    trades = []; i = W + ATR_N; cool = 0
    while i < n - 1:
        if cool > 0: cool -= 1; i += 1; continue
        a = atr[i]
        if a <= 0: i += 1; continue
        move = c[i] - c[i - W]
        strength = move / a
        side = 0
        if strength >= k: side = 1
        elif strength <= -k: side = -1
        if side == 0: i += 1; continue
        side *= mode    # fade = trade against the spike
        # enter NEXT bar open, pay entry slippage (adverse)
        ent_i = i + 1
        entry = o[ent_i] * (1 + side * ENTRY_SLIP_BPS / 10000.0)
        peak = entry; stop = entry - side * STOP_ATR * a
        tp = (entry + side * fade_tp * a) if fade_tp else None
        exit_px = None; j = ent_i
        while j < n and (j - ent_i) < max_hold:
            # end-of-day flat (intraday reactor) -- disabled for swing hold
            eod = eod_flat and ((j + 1 >= n) or (day_of(ts[j+1]) != day_of(ts[ent_i])))
            hi, lo = h[j], l[j]
            if side == 1:
                if lo <= stop: exit_px = stop; break          # hard stop (SL-first)
                if tp is not None and hi >= tp: exit_px = tp; break   # MR take-profit
                peak = max(peak, hi); trail = peak - TRAIL_ATR * a
                if tp is None and lo <= trail: exit_px = trail; break
            else:
                if hi >= stop: exit_px = stop; break
                if tp is not None and lo <= tp: exit_px = tp; break
                peak = min(peak, lo); trail = peak + TRAIL_ATR * a
                if tp is None and hi >= trail: exit_px = trail; break
            if eod: exit_px = c[j]; break
            j += 1
        if exit_px is None: exit_px = c[min(j, n-1)]
        gross_bps = side * (exit_px - entry) / entry * 10000.0
        net_bps = gross_bps - (EXIT_SLIP_BPS + 2 * COMMISSION_BPS)  # entry slip already in fill
        trades.append((ts[ent_i], net_bps))
        i = j + COOLDOWN; cool = 0
    return trades

def stats(trades):
    if not trades: return None
    trades.sort()
    split = trades[len(trades)//2][0]
    def agg(rows):
        n = len(rows); wins = sum(1 for _,p in rows if p > 0)
        gw = sum(p for _,p in rows if p > 0); gl = -sum(p for _,p in rows if p <= 0)
        pf = gw/gl if gl > 0 else (99 if gw > 0 else 0)
        return n, (100*wins/n if n else 0), pf, sum(p for _,p in rows)
    n, wr, pf, net = agg(trades)
    _,_,h1pf,_ = agg([t for t in trades if t[0] < split])
    _,_,h2pf,_ = agg([t for t in trades if t[0] >= split])
    # equity-curve maxDD in bps
    eq = 0.0; pk = 0.0; mdd = 0.0
    for _, p in trades:
        eq += p; pk = max(pk, eq); mdd = max(mdd, pk - eq)
    return dict(n=n, wr=wr, pf=pf, net=net, h1=h1pf, h2=h2pf, mdd=mdd, exp=net/n)

def main():
    d = sys.argv[1] if len(sys.argv) > 1 else "/tmp/stocks15m"
    files = sorted(glob.glob(os.path.join(d, "*_15m.csv")))
    print(f"# spike-reactor sweep | {len(files)} names | cost {COST_RT_BPS:.0f}bps RT | "
          f"trail {TRAIL_ATR}ATR stop {STOP_ATR}ATR maxhold {MAX_HOLD} | EQUITY 15m (BULL window)\n")
    configs = [
        ("CONTINUATION (chase, wide trail)", dict(mode=1,  fade_tp=None)),
        ("FADE (counter, wide trail)",       dict(mode=-1, fade_tp=None)),
        ("FADE (counter, MR take-profit 1.0ATR)", dict(mode=-1, fade_tp=1.0)),
        ("SWING (spike->multi-day hold, BigCapMomo-style, no eod flat)",
         dict(mode=1, fade_tp=None, max_hold=130, eod_flat=False)),
    ]
    for label, kw in configs:
        print(f"\n===== {label} =====")
        print(f"{'W(min)':>7} {'k*ATR':>6} {'n':>6} {'WR%':>5} {'PF':>5} {'netbps':>8} "
              f"{'exp':>6} {'H1pf':>5} {'H2pf':>5} {'maxDDbps':>9}  verdict")
        for W in (1, 2, 3, 4):
            for k in (1.5, 2.0, 2.5, 3.0):
                all_tr = []
                for f in files: all_tr.extend(run_name(f, W, k, **kw))
                s = stats(all_tr)
                if not s: print(f"{W*15:>7} {k:>6} {'--':>6}"); continue
                v = "*** both-halves+" if (s['h1']>1.0 and s['h2']>1.0 and s['net']>0) else \
                    ("+ net" if s['net']>0 else "DEAD")
                print(f"{W*15:>7} {k:>6} {s['n']:>6} {s['wr']:>5.1f} {s['pf']:>5.2f} "
                      f"{s['net']:>8.0f} {s['exp']:>6.1f} {s['h1']:>5.2f} {s['h2']:>5.2f} "
                      f"{s['mdd']:>9.0f}  {v}")

if __name__ == "__main__":
    main()
