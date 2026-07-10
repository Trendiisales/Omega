#!/usr/bin/env python3
"""
BIGCAP stock-mimic DRAWDOWN-CANCEL (lc) sweep (S-2026-07-11 operator item).

Purpose: a mimic never touches the parent trade, so an aggressive cold-loss cut on a
mimic leg is FREE protection (opposite of the trend-engine rule where tightening hurts).
This sweep defines the universal drawdown-cancel LEVER for the stock x4 mimic (and, by the
"enforce it on ALL mimics" instruction, the reference lever every active mimic must carry).

Reuses the faithful bigcap_upjump_ladder_bt harness (load/prep/parent_trades/metrics) on
the SURVIVOR config from the base sweep (TIGHT a0.5/s2/g0, WIDE a8/s0/g50, cap5 reclip5%).
Adds lc = drawdown-cancel: a leg whose price falls lc% below its live cost-base (le) is
cut and banked at the loss. lc=0 == the base ladder (no cut, current behaviour).

Judged STANDALONE / additive (CompanionDominanceError): NEVER compared to riding WIDE.
Report per-lc: net%, PF, WF halves, bull/bear, worst clip. Pick the lc that keeps the
all-6 gate, lifts worst (protection), and does not gut net (mimic edge preserved).
"""
import bigcap_upjump_ladder_bt as B

RECLIP = B.RECLIP; CAP = B.CAP; RT_BP = B.RT_BP


class Leg:
    __slots__ = ("epx", "le", "arm", "sb", "gb", "rc", "lc", "open", "clipped", "pk", "mfe", "ext")
    def __init__(self, entry_px, arm, stall, gb, reclip, lc):
        self.epx = entry_px; self.le = entry_px; self.arm = arm; self.sb = stall
        self.gb = gb; self.rc = reclip; self.lc = lc
        self.open = False; self.clipped = False
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
        # DRAWDOWN-CANCEL (free protection: mimic never touches parent). Cut a leg that
        # falls lc% below its live cost-base. Fires regardless of arm state (cold cut).
        if self.lc > 0 and (cur / self.le - 1.0) * 100.0 <= -self.lc:
            self.pk = self.mfe; self.clipped = True
            return (cur / self.le - 1.0) * 1e4
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


def main():
    _, series = B.load()
    prepped = {}
    for s in series:
        c, sma = B.prep(series[s])
        prepped[s] = (c, sma, B.parent_trades(c))
    npar = sum(len(t) for _, _, t in prepped.values())
    print(f"universe {len(prepped)} names, {npar} parent events (thr {B.THR*100:.0f}% day)")

    # SURVIVOR config from the base all-6 sweep.
    TIGHT = (0.5, 2, 0.0); WIDE = (8.0, 0, 0.50)

    def book(lc, rt):
        rows = []
        t = (*TIGHT, RECLIP, lc); w = (*WIDE, RECLIP, lc)
        for s, (c, sma, trades) in prepped.items():
            for ei, xi in trades:
                bull = (sma[ei] is not None and c[ei] is not None and c[ei] > sma[ei])
                for net in run_trade(c, ei, xi, t, w, rt):
                    rows.append((xi, net / 100.0, bull, s))
        return rows

    print("\nSURVIVOR TIGHT a0.5/s2/g0  WIDE a8/s0/g50  cap5 reclip5%  (units = % of clip notional)")
    print("drawdown-cancel (lc) sweep — all-6 gate: net>0 PF>1 H1>0 H2>0 bear>=0\n")
    print(f"  {'lc%':>5} {'n':>5} {'net%':>9} {'PF':>5} {'H1':>8} {'H2':>8} "
          f"{'bull':>8} {'bear':>8} {'worst':>8} {'2x-net':>9} {'pass':>5}")
    for lc in (0, 5, 8, 10, 12, 15, 20, 25):
        m = B.metrics(book(lc, RT_BP))
        m2 = B.metrics(book(lc, RT_BP * 2))
        print(f"  {lc:>5} {m['n']:>5} {m['net']:>+9.0f} {m['pf']:>5.2f} {m['h1']:>+8.0f} {m['h2']:>+8.0f} "
              f"{m['bull']:>+8.0f} {m['bear']:>+8.0f} {m['worst']:>+8.0f} {m2['net']:>+9.0f} "
              f"{'Y' if m['passed'] else '.':>5}")


if __name__ == "__main__":
    main()
