#!/usr/bin/env python3
"""WIDE-runner TRAIL-TIGHTEN sweep on the BIGCAP daily UP-JUMP LADDER
(StockDayMoverLadderCompanion sibling), operator pivot 2026-07-09.

Faithful to the LIVE sibling live_step_() (daily-close cadence): parent +thr day -> enter
NEXT close; base legs = TIGHT(arm0.5, stall2, gb0) + MIRROR(arm2, gb0.75) + WIDE(arm8, gb0.5);
self-funding ladder spawns WIDE legs on a net-positive clip (cap 5); reclip on trend resume;
LOSS_CUT at -15% fav; -thr day -> flush NEXT close. Only the WIDE runner (and the self-funded
WIDE spawns) get the tightened arm+gb; TIGHT + MIRROR unchanged.

Baseline = wired config w_arm 8 / w_gb 0.50. Sweep w_arm {1,2,3,4} x w_gb {0.15,0.20,0.25,0.30}.
Pooled over the 39 BIGCAP names, all-6 gate + round-trip / avg-giveback. Units = % of notional.
"""
import csv
from pathlib import Path

DATA = Path(__file__).resolve().parent.parent / "data/rdagent/sp500_long_close.csv"
BIGCAP = ("NVDA AMD AVGO MU MRVL SMCI ARM PLTR TSLA META NFLX CRWD SHOP COIN MSTR "
          "SNOW NOW PANW UBER ABNB DELL ORCL QCOM INTC AMZN GOOGL MSFT AAPL CRM ADBE "
          "IONQ RGTI QBTS ASTS RKLB NBIS CRWV ALAB CRDO").split()
RT_BP = 8.0; SMA_N = 200; THR = 0.03; RECLIP = 0.05; CAP = 5; LOSS_CUT = 15.0

def load():
    with open(DATA) as f:
        r = csv.reader(f); hdr = next(r)
        idx = {s: i for i, s in enumerate(hdr) if s in BIGCAP}
        series = {s: [] for s in idx}
        for row in r:
            for s, i in idx.items():
                v = row[i]; series[s].append(float(v) if v not in ("", None) else None)
    return series

def prep(px):
    c = px; sma = [None] * len(c); run = 0.0; buf = []
    for i, v in enumerate(c):
        if v is None: continue
        buf.append(v); run += v
        if len(buf) > SMA_N: run -= buf.pop(0)
        if len(buf) == SMA_N: sma[i] = run / SMA_N
    return c, sma

def parent_trades(c):
    n = len(c); trades = []; pos = False; ei = None
    valid = [i for i in range(n) if c[i] is not None]
    for k in range(1, len(valid)):
        i, p = valid[k], valid[k - 1]; j = c[i] / c[p] - 1.0
        if not pos and j >= THR:
            if k + 1 < len(valid): pos = True; ei = valid[k + 1]
        elif pos and j <= -THR:
            xi = valid[k + 1] if k + 1 < len(valid) else valid[k]
            if xi > ei: trades.append((ei, xi)); pos = False; ei = None
    if pos and ei is not None and valid[-1] > ei: trades.append((ei, valid[-1]))
    return trades

class Leg:
    __slots__ = ("epx", "le", "arm", "sb", "gb", "open", "clipped", "pk", "mfe", "ext", "kind")
    def __init__(self, entry_px, arm, stall, gb, kind):
        self.epx = entry_px; self.le = entry_px; self.arm = arm; self.sb = stall
        self.gb = gb; self.open = False; self.clipped = False; self.pk = 0.0
        self.mfe = 0.0; self.ext = 0; self.kind = kind
    def step(self, bar, cur):
        fav = (cur - self.epx) / self.epx * 100.0
        if self.clipped:
            if RECLIP > 0 and self.pk > 0 and fav > self.pk * (1.0 + RECLIP):
                self.clipped = False; self.le = cur
            else: return None
        if not self.open: self.open = True; self.mfe = fav; self.ext = bar
        if fav > self.mfe + 1e-9: self.mfe = fav; self.ext = bar
        if LOSS_CUT > 0 and fav <= -LOSS_CUT:
            self.clipped = True; self.pk = self.mfe
            return ("LC", (cur / self.le - 1.0) * 1e4, self.mfe, fav)
        armed = self.mfe >= self.arm; stall = bar - self.ext
        if armed and self.sb > 0 and stall >= self.sb:
            self.pk = self.mfe; self.clipped = True
            return ("STALL", (cur / self.le - 1.0) * 1e4, self.mfe, fav)
        if armed and self.gb > 0 and fav <= self.mfe * (1.0 - self.gb):
            self.pk = self.mfe; self.clipped = True
            return ("GB", (cur / self.le - 1.0) * 1e4, self.mfe, fav)
        return None

def run_trade(c, ei, xi, w_arm, w_gb, rt_bp):
    epx = c[ei]
    legs = [Leg(epx, 0.5, 2, 0.0, "T"), Leg(epx, 2.0, 0, 0.75, "M"), Leg(epx, w_arm, 0, w_gb, "W")]
    spawned = 3; recs = []
    for i in range(ei, xi):
        if c[i] is None: continue
        cur = c[i]; new = []
        for lg in legs:
            r = lg.step(i, cur)
            if r is not None:
                _, g, mfe, fav = r; net = g - rt_bp
                recs.append((net, mfe, fav))       # net bp, MFE%, exit fav%
                if net > 0 and spawned < CAP:
                    new.append(Leg(cur, w_arm, 0, w_gb, "W")); spawned += 1
        legs += new
    last = None
    for i in range(xi, ei, -1):
        if c[i] is not None: last = c[i]; break
    if last is None: last = epx
    for lg in legs:
        if lg.open and not lg.clipped:
            fav = (last / lg.epx - 1.0) * 100.0
            recs.append(((last / lg.le - 1.0) * 1e4 - rt_bp, lg.mfe, fav))
    return recs

def metrics(rows):
    # rows: (xi, net_pct, mfe_pct, exit_fav_pct, bull)
    if not rows: return None
    net = sum(r[1] for r in rows)
    gw = sum(r[1] for r in rows if r[1] > 0); gl = -sum(r[1] for r in rows if r[1] < 0)
    pf = gw / gl if gl > 0 else (999 if gw > 0 else 0)
    ex = sorted(r[0] for r in rows); mid = ex[len(ex) // 2]
    h1 = sum(r[1] for r in rows if r[0] < mid); h2 = sum(r[1] for r in rows if r[0] >= mid)
    bull = sum(r[1] for r in rows if r[4]); bear = sum(r[1] for r in rows if not r[4])
    worst = min(r[1] for r in rows); exbest = net - max(r[1] for r in rows)
    n = len(rows)
    rt = 100.0 * sum(1 for r in rows if r[2] >= 2.0 and r[3] <= 0.0) / n
    gbs = [r[2] - r[3] for r in rows if r[3] > 0.0]
    avggb = sum(gbs) / len(gbs) if gbs else 0.0
    caps = [r[3] / r[2] for r in rows if r[3] > 0.0 and r[2] > 0.05]
    cap = 100.0 * sum(caps) / len(caps) if caps else 0.0
    passed = (net > 0 and pf > 1 and h1 > 0 and h2 > 0 and bear >= 0)
    return dict(n=n, net=net, pf=pf, h1=h1, h2=h2, bull=bull, bear=bear, worst=worst,
                exbest=exbest, rt=rt, avggb=avggb, cap=cap, passed=passed)

def book(prepped, w_arm, w_gb, rt):
    rows = []
    for s, (c, sma, trades) in prepped.items():
        for ei, xi in trades:
            bull = (sma[ei] is not None and c[ei] is not None and c[ei] > sma[ei])
            for net, mfe, fav in run_trade(c, ei, xi, w_arm, w_gb, rt):
                rows.append((xi, net / 100.0, mfe, fav, bull))
    return rows

def main():
    series = load()
    prepped = {}
    for s in series:
        c, sma = prep(series[s]); prepped[s] = (c, sma, parent_trades(c))
    npar = sum(len(t) for _, _, t in prepped.values())
    print(f"BIGCAP daily up-jump ladder: {len(prepped)} names, {npar} parent windows (thr {THR*100:.0f}%)\n")
    base = metrics(book(prepped, 8.0, 0.50, RT_BP))
    b2 = metrics(book(prepped, 8.0, 0.50, RT_BP * 2))
    print(f"BASELINE w_arm8/w_gb0.50: n={base['n']} net={base['net']:+.0f}% PF={base['pf']:.2f} "
          f"H1={base['h1']:+.0f}/H2={base['h2']:+.0f} bull={base['bull']:+.0f} bear={base['bear']:+.0f} "
          f"worst={base['worst']:+.0f} | RT={base['rt']:.2f}% avgGB={base['avggb']:.2f}pt cap={base['cap']:.1f}% exBest={base['exbest']:+.0f} 2x={b2['net']:+.0f}")
    print(f"\n  {'w_arm':>5} {'w_gb':>4} | {'net%':>8} {'dNet%':>7} {'PF':>5} {'pass':>4} {'H1':>6} {'H2':>6} "
          f"{'bear':>6} {'RT%':>6} {'dRT':>6} {'avgGB':>6} {'cap%':>6} {'worst':>6} {'2x':>7}")
    for w_arm in (0.0, 1.0, 2.0, 3.0):
        for w_gb in (0.10, 0.15, 0.20):
            m = metrics(book(prepped, w_arm, w_gb, RT_BP))
            m2 = metrics(book(prepped, w_arm, w_gb, RT_BP * 2))
            dnet = 100.0 * (m['net'] - base['net']) / abs(base['net']) if base['net'] else 0.0
            drt = m['rt'] - base['rt']
            print(f"  {w_arm:>5.1f} {w_gb:>4.2f} | {m['net']:+8.0f} {dnet:+6.1f}% {m['pf']:5.2f} "
                  f"{'YES' if m['passed'] else 'no':>4} {m['h1']:+6.0f} {m['h2']:+6.0f} {m['bear']:+6.0f} "
                  f"{m['rt']:5.2f}% {drt:+6.2f} {m['avggb']:6.2f} {m['cap']:5.1f}% {m['worst']:+6.0f} {m2['net']:+7.0f}")

if __name__ == '__main__':
    main()
