#!/usr/bin/env python3
"""
BIGCAP daily-mover UP-JUMP LADDER (S-2026-07-07 operator item 4).

Prior art: StockDayMoverBeFloorCompanion RETIRED S-2026-07-07e (BE-floor real-fill
-$110.7k vs +$1.57M model — the floor died). This tests the NO-FLOOR giveback ladder
(the crypto tiered-ladder mechanism, the only trail family that survives real fills
per the S-2026-07-07 late-arm sweep) on the same universe/data.

Mechanism = faithful port of crypto_upjump_tiered_ladder_sweep.py Leg/run_trade on
DAILY closes (data/rdagent/sp500_long_close.csv, 2019-06..2026-06, 39 BIGCAP names):
  parent: +thr% day close-to-close -> enter NEXT close (real fill); exit on -thr% day
          (next close); end-flush MTM.
  legs:   2 base tiers (tight banks, wide rides) + self-funding ladder cap 5,
          arm on MFE from entry, giveback trail from peak, reclip on trend resume,
          flush MTM at parent exit (never abandoned). NO FLOOR anywhere.
Real fills: entries/exits AT daily closes, cost RT_BP debited per clip, 2x stress col.
Gate all-6 on the pooled 39-name book: net>0, PF>1, WF halves both>0, bear>=0
(200d SMA per name at parent entry). Loss protection = giveback trail from entry-peak
(mandate feedback-engine-loss-protection-provision) — worst clip reported.
"""
import csv, sys
from pathlib import Path

DATA = Path(__file__).resolve().parent.parent / "data/rdagent/sp500_long_close.csv"
BIGCAP = ("NVDA AMD AVGO MU MRVL SMCI ARM PLTR TSLA META NFLX CRWD SHOP COIN MSTR "
          "SNOW NOW PANW UBER ABNB DELL ORCL QCOM INTC AMZN GOOGL MSFT AAPL CRM ADBE "
          "IONQ RGTI QBTS ASTS RKLB NBIS CRWV ALAB CRDO").split()
RT_BP = 8.0
SMA_N = 200
THR = 0.03
RECLIP = 0.05
CAP = 5

def load():
    with open(DATA) as f:
        r = csv.reader(f)
        hdr = next(r)
        idx = {s: i for i, s in enumerate(hdr) if s in BIGCAP}
        series = {s: [] for s in idx}
        dates = []
        for row in r:
            dates.append(row[0])
            for s, i in idx.items():
                v = row[i]
                series[s].append(float(v) if v not in ("", None) else None)
    return dates, series

def prep(px):
    # forward-index valid closes; sma200 over valid history
    c = px
    sma = [None] * len(c)
    run = 0.0; cnt = 0; buf = []
    for i, v in enumerate(c):
        if v is None: continue
        buf.append(v); run += v
        if len(buf) > SMA_N: run -= buf.pop(0)
        if len(buf) == SMA_N: sma[i] = run / SMA_N
    return c, sma

def parent_trades(c):
    """(entry_i, exit_i) with entry at NEXT valid close after +THR day; exit next close after -THR day."""
    n = len(c); trades = []; pos = False; ei = None
    prev_i = None
    valid = [i for i in range(n) if c[i] is not None]
    for k in range(1, len(valid)):
        i, p = valid[k], valid[k - 1]
        j = c[i] / c[p] - 1.0
        if not pos and j >= THR:
            if k + 1 < len(valid):
                pos = True; ei = valid[k + 1]
        elif pos and j <= -THR:
            xi = valid[k + 1] if k + 1 < len(valid) else valid[k]
            if xi > ei: trades.append((ei, xi))
            pos = False; ei = None
    if pos and ei is not None and valid[-1] > ei:
        trades.append((ei, valid[-1]))
    return trades

class Leg:
    __slots__ = ("epx", "le", "arm", "sb", "gb", "rc", "open", "clipped", "pk", "mfe", "ext")
    def __init__(self, entry_px, arm, stall, gb, reclip):
        self.epx = entry_px; self.le = entry_px; self.arm = arm; self.sb = stall
        self.gb = gb; self.rc = reclip; self.open = False; self.clipped = False
        self.pk = 0.0; self.mfe = 0.0; self.ext = 0
    def step(self, bar, cur):
        fav = (cur - self.epx) / self.epx * 100.0
        if self.clipped:
            if self.rc > 0 and self.pk > 0 and fav > self.pk * (1.0 + self.rc):
                self.clipped = False; self.le = cur
            else: return None
        if not self.open:
            self.open = True; self.mfe = fav; self.ext = bar
        if fav > self.mfe + 1e-9:
            self.mfe = fav; self.ext = bar
        armed = self.mfe >= self.arm; stall = bar - self.ext
        if armed and self.sb > 0 and stall >= self.sb:
            self.pk = self.mfe; self.clipped = True
            return (cur / self.le - 1.0) * 1e4
        if armed and self.gb > 0 and fav <= self.mfe * (1.0 - self.gb):
            self.pk = self.mfe; self.clipped = True
            return (cur / self.le - 1.0) * 1e4
        return None

def run_trade(c, ei, xi, tight, wide, rt_bp):
    epx = c[ei]
    legs = [Leg(epx, *tight), Leg(epx, *wide)]
    spawned = 2; clips = []
    for i in range(ei, xi):
        if c[i] is None: continue
        cur = c[i]; new = []
        for lg in legs:
            g = lg.step(i, cur)
            if g is not None:
                net = g - rt_bp
                clips.append(net)
                if net > 0 and spawned < CAP:
                    new.append(Leg(cur, *wide)); spawned += 1
        legs += new
    last = None
    for i in range(xi, ei, -1):
        if c[i] is not None: last = c[i]; break
    if last is None: last = epx
    for lg in legs:
        if lg.open and not lg.clipped:
            clips.append((last / lg.le - 1.0) * 1e4 - rt_bp)
    return clips

def metrics(rows):
    if not rows: return dict(n=0, net=0, pf=0, h1=0, h2=0, bull=0, bear=0, worst=0, passed=False)
    net = sum(r[1] for r in rows)
    gw = sum(r[1] for r in rows if r[1] > 0); gl = -sum(r[1] for r in rows if r[1] < 0)
    pf = gw / gl if gl > 0 else (999 if gw > 0 else 0)
    ex = sorted(r[0] for r in rows); mid = ex[len(ex) // 2]
    h1 = sum(r[1] for r in rows if r[0] < mid); h2 = sum(r[1] for r in rows if r[0] >= mid)
    bull = sum(r[1] for r in rows if r[2]); bear = sum(r[1] for r in rows if not r[2])
    worst = min(r[1] for r in rows)
    passed = (net > 0 and pf > 1 and h1 > 0 and h2 > 0 and bear >= 0)
    return dict(n=len(rows), net=net, pf=pf, h1=h1, h2=h2, bull=bull, bear=bear, worst=worst, passed=passed)

def main():
    dates, series = load()
    prepped = {}
    for s in series:
        c, sma = prep(series[s])
        prepped[s] = (c, sma, parent_trades(c))
    npar = sum(len(t) for _, _, t in prepped.values())
    print(f"universe {len(prepped)} names, {npar} parent events (thr {THR*100:.0f}% day, entry next close)")

    TIGHT = [(a, st, gb) for a in (0.5, 1.0, 2.0, 3.0) for (st, gb) in ((0, 0.30), (0, 0.50), (2, 0.0), (3, 0.0), (3, 0.30))]
    WIDE  = [(a, st, gb) for a in (4.0, 6.0, 8.0)      for (st, gb) in ((0, 0.40), (0, 0.50), (4, 0.0), (6, 0.0), (6, 0.50))]

    def book(tight, wide, rt):
        rows = []
        for s, (c, sma, trades) in prepped.items():
            for ei, xi in trades:
                bull = (sma[ei] is not None and c[ei] is not None and c[ei] > sma[ei])
                for net in run_trade(c, ei, xi, (*tight, RECLIP), (*wide, RECLIP), rt):
                    rows.append((xi, net / 100.0, bull, s))
        return rows

    best = None
    for t in TIGHT:
        for w in WIDE:
            m = metrics(book(t, w, RT_BP))
            if m["passed"] and (best is None or m["net"] > best[2]["net"]):
                best = (t, w, m)
    def cfgs(c): return f"a{c[0]:g}/s{c[1]}/g{int(c[2]*100)}"
    print("\npooled 39-name book, all-6 gate (net/PF/H1/H2/bear>=0), units = % of clip notional")
    if best is None:
        print("NO CONFIG PASSES ALL-6 -> BIGCAP upjump ladder DEAD on this universe")
        # show best net anyway for the record
        allm = []
        for t in TIGHT:
            for w in WIDE:
                m = metrics(book(t, w, RT_BP))
                allm.append((m["net"], t, w, m))
        allm.sort(reverse=True)
        for net, t, w, m in allm[:5]:
            print(f"  best-net {cfgs(t):12s} {cfgs(w):12s} n={m['n']:5d} net={m['net']:+8.0f}% PF={m['pf']:4.2f} "
                  f"H1={m['h1']:+7.0f} H2={m['h2']:+7.0f} bull={m['bull']:+7.0f} bear={m['bear']:+7.0f} worst={m['worst']:+6.0f}bp")
        return
    t, w, m = best
    m2 = metrics(book(t, w, RT_BP * 2))
    print(f"SURVIVOR: TIGHT {cfgs(t)}  WIDE {cfgs(w)}  ladder-cap{CAP} reclip{int(RECLIP*100)}%")
    print(f"  n={m['n']} net={m['net']:+.0f}% PF={m['pf']:.2f} H1={m['h1']:+.0f} H2={m['h2']:+.0f} "
          f"bull={m['bull']:+.0f} bear={m['bear']:+.0f} worst={m['worst']:+.0f}bp")
    print(f"  2x-cost: net={m2['net']:+.0f}% PF={m2['pf']:.2f} pass={m2['passed']}")
    # neighbors (plateau check)
    print("  neighbors (same exits, arms +/-1 step):")
    ta = [0.5, 1.0, 2.0, 3.0]; wa = [4.0, 6.0, 8.0]
    for dt in (-1, 0, 1):
        for dw in (-1, 0, 1):
            if dt == 0 and dw == 0: continue
            i, j = ta.index(t[0]) + dt, wa.index(w[0]) + dw
            if not (0 <= i < len(ta) and 0 <= j < len(wa)): continue
            t2 = (ta[i], t[1], t[2]); w2 = (wa[j], w[1], w[2])
            mm = metrics(book(t2, w2, RT_BP))
            print(f"    {cfgs(t2):12s} {cfgs(w2):12s} net={mm['net']:+8.0f}% PF={mm['pf']:4.2f} pass={mm['passed']}")
    # per-name split of the survivor
    rows = book(t, w, RT_BP)
    per = {}
    for xi, net, bull, s in rows: per.setdefault(s, []).append(net)
    ranked = sorted(per.items(), key=lambda kv: -sum(kv[1]))
    print("  per-name (top 8 / bottom 8):")
    for s, v in ranked[:8]: print(f"    {s:6s} n={len(v):4d} net={sum(v):+8.1f}%")
    for s, v in ranked[-8:]: print(f"    {s:6s} n={len(v):4d} net={sum(v):+8.1f}%")

if __name__ == "__main__":
    main()
