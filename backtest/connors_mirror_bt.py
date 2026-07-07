#!/usr/bin/env python3
"""
ConnorsMirror x2 companion — REAL-FILL backtest (S-2026-07-07 operator ask).

Parent: REAL ConnorsRSI2Engine trades (backtest/connors_mirror_dump.cpp ->
/tmp/connors_parent_trades.csv, deployed config REGIME_GATE=1, NDX daily).
Mirror: SEPARATE INDEPENDENT book (feedback-companion-independent-engine).
While a parent trade is open, watch NAS100 H1. When price gains >= ARM over
the parent entry, open an x2-size mirror LONG at that H1 close (real fill).
Tight trail: giveback GB from the running peak. Exec modes:
  close    — exit fills at the triggering H1 CLOSE (worse-of, registry §5)
  intrabar — resting stop at peak*(1-GB); fills AT the stop level, or at the
             bar OPEN when the bar gaps through the stop (gap honesty)
Force-flat at parent exit (signal gone). Costs: NAS100 real rt_bp=3.0
(engine_init table), debited per round trip; 2x-cost robustness pass.
Judged STANDALONE (never vs parent/WIDE). Gates: net+, PF, both WF halves +,
bull(2024-26)+, bear(2022)+, maxDD.

run: TICKDIR=~/Tick python3 backtest/connors_mirror_bt.py
"""
import os, sys, csv, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.environ.setdefault("TICKDIR", os.path.expanduser("~/Tick"))
import rider_sweep_wf as RW

RT_BP = 3.0          # NAS100 real round-trip cost, engine_init rt_bp table
PARENT_CSV = "/tmp/connors_parent_trades.csv"

def load_parents(path):
    out = []
    with open(path) as f:
        for r in csv.DictReader(f):
            out.append((int(r["entry_ts"]), int(r["exit_ts"]),
                        float(r["entry_px"]), float(r["exit_px"]), float(r["size"])))
    return out

def year_of(ts): return time.gmtime(ts).tm_year

def day_of(ts):  return ts // 86400

def sim(parents, H1, arm, gb, mode, rt_bp, retrig):
    """One mirror config. Returns list of (entry_ts, pnl_pts_per_unit_net)."""
    ts, o, h, l, c = H1
    import bisect
    trades = []
    for (ets, xts, epx, xpx, psz) in parents:
        # window: H1 bars strictly after parent entry DAY through parent exit DAY (UTC dates)
        d0, d1 = day_of(ets), day_of(xts)
        i = bisect.bisect_left(ts, (d0 + 1) * 86400)
        j = bisect.bisect_right(ts, (d1 + 1) * 86400 - 1)
        if i >= j: continue
        armed = False; peak = 0.0; en = 0.0; last_clip_peak = None; done = False
        for k in range(i, j):
            if not armed:
                if done and not retrig:
                    break                       # ONE mirror per parent trade
                if done:                        # re-arm only on +retrig above prior peak
                    trig = last_clip_peak * (1.0 + retrig)
                else:
                    trig = epx * (1.0 + arm)
                if c[k] >= trig:
                    armed = True; en = c[k]; peak = c[k]; ent_ts = ts[k]
                continue
            stop = peak * (1.0 - gb)
            fill = None
            if mode == "intrabar":
                if o[k] <= stop:   fill = o[k]          # gap through -> open
                elif l[k] <= stop: fill = stop          # resting stop at level
            else:
                if c[k] <= stop:   fill = min(c[k], stop)  # close-eval, worse-of
            if fill is not None:
                pnl = (fill - en) - en * rt_bp * 1e-4
                trades.append((ent_ts, pnl))
                armed = False; last_clip_peak = peak; done = True
                continue
            if c[k] > peak: peak = c[k]
        if armed:  # parent exit -> flat at last window close
            fill = c[j - 1]
            pnl = (fill - en) - en * rt_bp * 1e-4
            trades.append((ent_ts, pnl))
    return trades

def stat(trades):
    n = len(trades)
    if n == 0: return dict(n=0, net=0, pf=0, wr=0, dd=0, bh=False, bull=0, bear=0, npos=0)
    gp = sum(p for _, p in trades if p > 0); gl = sum(-p for _, p in trades if p <= 0)
    net = gp - gl; w = sum(1 for _, p in trades if p > 0)
    eq = pk = dd = 0.0
    for _, p in trades:
        eq += p; pk = max(pk, eq); dd = max(dd, pk - eq)
    half = n // 2
    h1 = sum(p for _, p in trades[:half]); h2 = sum(p for _, p in trades[half:])
    bull = sum(p for t, p in trades if year_of(t) >= 2024)
    bear = sum(p for t, p in trades if year_of(t) == 2022)
    nbear = sum(1 for t, p in trades if year_of(t) == 2022)
    return dict(n=n, net=net, pf=(gp / gl if gl > 0 else 999), wr=100.0 * w / n,
                dd=dd, bh=(h1 > 0 and h2 > 0), h1=h1, h2=h2, bull=bull, bear=bear, nbear=nbear)

def main():
    parents = load_parents(PARENT_CSV)
    print(f"parents: {len(parents)} trades {time.strftime('%Y-%m-%d', time.gmtime(parents[0][0]))}"
          f" .. {time.strftime('%Y-%m-%d', time.gmtime(parents[-1][1]))}")
    H1, src = RW.find_data("NAS100")
    print(f"NAS100 H1: {len(H1[0])} bars ({src})  "
          f"{time.strftime('%Y-%m-%d', time.gmtime(H1[0][0]))} .. {time.strftime('%Y-%m-%d', time.gmtime(H1[0][-1]))}")
    # coverage: parents whose window has H1 data
    ts0, ts1 = H1[0][0], H1[0][-1]
    cov = [p for p in parents if p[0] >= ts0 - 86400 and p[1] <= ts1 + 86400]
    print(f"parents inside H1 coverage: {len(cov)}")

    ARMS = [0.004, 0.006, 0.010, 0.015, 0.020]
    GBS  = [0.003, 0.005, 0.0075, 0.010]
    print(f"\n{'mode':8s} {'arm%':>5s} {'gb%':>5s} {'ret':>3s} | {'n':>4s} {'net_pt':>9s} {'PF':>5s} "
          f"{'WR%':>5s} {'DD':>8s} {'bothH':>5s} {'H1':>8s} {'H2':>8s} {'bull24+':>8s} {'bear22':>8s} {'nbr':>3s} | {'net@2xcost':>10s}")
    for mode in ("close", "intrabar"):
        for arm in ARMS:
            for gb in GBS:
                for retrig in (0.0, 0.02):
                    tr = sim(cov, H1, arm, gb, mode, RT_BP, retrig)
                    s = stat(tr)
                    if s["n"] == 0: continue
                    tr2 = sim(cov, H1, arm, gb, mode, RT_BP * 2, retrig)
                    s2 = stat(tr2)
                    print(f"{mode:8s} {arm*100:5.2f} {gb*100:5.2f} {('Y' if retrig else '-'):>3s} | "
                          f"{s['n']:4d} {s['net']:9.1f} {s['pf']:5.2f} {s['wr']:5.1f} {s['dd']:8.1f} "
                          f"{('YES' if s['bh'] else 'no'):>5s} {s['h1']:8.1f} {s['h2']:8.1f} "
                          f"{s['bull']:8.1f} {s['bear']:8.1f} {s['nbear']:3d} | {s2['net']:10.1f}")

if __name__ == "__main__":
    main()
