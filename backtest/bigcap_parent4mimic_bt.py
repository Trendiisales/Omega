#!/usr/bin/env python3
"""
BIGCAP PARENT + 4x BE-MIMIC redo (operator S-2026-07-13: "the bigcap engine should be
a engine that trades on trigger with 4x mimic engines" — no immediate-entry legs
beyond the ONE parent; every other leg books NOTHING until BE covered).

Faithful extension of the VALIDATED bigcap_upjump_ladder_bt.py (same feed, same
parent event definition, same close-fill convention, RT 8bp, lc15):

  PARENT (the engine, trades on trigger — allowed immediate entry):
    +thr% day close-to-close -> enter NEXT close; peak-trail arm1/gb10
    (S-2026-07-09 wide-trail cell) + lc15; flush MTM at -thr% day exit.

  4x MIMIC legs (companion-at-BE, feedback-no-immediate-entry-upjump-mimic-only):
    spawn PENDING at the parent entry; a leg opens ONLY at the first close
    >= trigger*(1+be%) (cost/BE covered — close-grade, the engine manages on
    daily closes); cancel if BE not made within pend closes. Then the validated
    cell trails: T a0.5/s2 (stall banker), M a2/gb75 (mirror), W a1/gb10
    (wide trail), W8 a8/gb50 (old runner). lc15 from the LEG entry. Reclip 5%
    (re-enter after a NEW favorable extreme = "re-enter after BE"), self-funding
    cap 5/window. Flush MTM at parent exit.

  Judged STANDALONE (mimic book separate from parent book) per
  feedback-companion-independent-engine. Gate: all-6 (n>=30, net>0, PF, WF both
  halves, bear>=0) + 2x-cost, pooled across ALL 45 wired names (never-half-the-
  symbols).
"""
import csv
from pathlib import Path

DATA = Path(__file__).resolve().parent.parent / "data/rdagent/sp500_long_close.csv"
# the EXACT 45-name wired universe (engine_init.hpp BIGCAP_LAD, S-2026-07-08c)
BIGCAP = ("NVDA AMD AVGO MU MRVL SMCI ARM PLTR TSLA META NFLX CRWD SHOP COIN MSTR "
          "SNOW NOW PANW UBER ABNB DELL ORCL QCOM INTC AMZN GOOGL MSFT AAPL CRM ADBE "
          "IONQ RGTI QBTS ASTS RKLB NBIS CRWV ALAB CRDO WDC STX DD TPR BMY SWKS").split()
RT_BP = 8.0
SMA_N = 200
THR = 0.03
RECLIP = 0.05
CAP = 5
LC_PCT = 15.0

# 4 mimic cells (validated plateau cells from the wired book): (arm%, stall_bars, gb_frac)
MIMIC4 = [("T", 0.5, 2, 0.0), ("M", 2.0, 0, 0.75), ("W", 1.0, 0, 0.10), ("W8", 8.0, 0, 0.50)]
PARENT_CELL = (1.0, 0, 0.10)   # the engine's own trail (S-2026-07-09 wide cell)


def load():
    with open(DATA) as f:
        r = csv.reader(f)
        hdr = next(r)
        idx = {s: i for i, s in enumerate(hdr) if s in BIGCAP}
        series = {s: [] for s in idx}
        for row in r:
            for s, i in idx.items():
                v = row[i]
                series[s].append(float(v) if v not in ("", None) else None)
    missing = [s for s in BIGCAP if s not in idx]
    return series, missing


def prep(px):
    c = px
    sma = [None] * len(c)
    run = 0.0; buf = []
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
    """Validated Leg mechanics + lc15 (wired loss_cut_pct) + BE-ENTRY pending gate."""
    __slots__ = ("trig", "epx", "le", "arm", "sb", "gb", "rc", "open", "clipped",
                 "pk", "mfe", "ext", "pending", "be", "pbars", "dead")

    def __init__(self, trigger_px, arm, stall, gb, reclip, be_pct=0.0, pend=0):
        self.trig = trigger_px; self.epx = trigger_px; self.le = trigger_px
        self.arm = arm; self.sb = stall; self.gb = gb; self.rc = reclip
        self.open = False; self.clipped = False; self.dead = False
        self.pk = 0.0; self.mfe = 0.0; self.ext = 0
        self.be = be_pct; self.pending = be_pct > 0.0; self.pbars = pend

    def step(self, bar, cur):
        if self.dead: return None
        if self.pending:
            # BE-ENTRY: open ONLY once a close covers +be% off the trigger (close-grade,
            # matching the daily-close engine). Cancel when the pend window expires.
            if (cur / self.trig - 1.0) * 100.0 >= self.be:
                self.pending = False
                self.epx = cur; self.le = cur   # enter AT this close (real fill)
                self.open = True; self.mfe = 0.0; self.ext = bar
                return None
            self.pbars -= 1
            if self.pbars <= 0: self.dead = True
            return None
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
        if not armed and fav <= -LC_PCT:   # DRAWDOWN-CANCEL (wired loss_cut_pct=15)
            self.dead = True
            return (cur / self.le - 1.0) * 1e4
        if armed and self.sb > 0 and stall >= self.sb:
            self.pk = self.mfe; self.clipped = True
            return (cur / self.le - 1.0) * 1e4
        if armed and self.gb > 0 and fav <= self.mfe * (1.0 - self.gb):
            self.pk = self.mfe; self.clipped = True
            return (cur / self.le - 1.0) * 1e4
        return None


def run_window(c, ei, xi, be_pct, pend, rt_bp):
    """One parent window. Returns (parent_clips, mimic_clips) net bp lists."""
    epx = c[ei]
    parent = Leg(epx, *PARENT_CELL, RECLIP)
    mimics = [Leg(epx, a, s, g, RECLIP, be_pct, pend) for (_, a, s, g) in MIMIC4]
    spawned = len(mimics); pclips = []; mclips = []
    wide = MIMIC4[2]   # ladder respawns use the W cell, BE-gated off the clip close
    extra = []
    for i in range(ei, xi):
        if c[i] is None: continue
        cur = c[i]
        g = parent.step(i, cur)
        if g is not None: pclips.append(g - rt_bp)
        new = []
        for lg in mimics + extra:
            g = lg.step(i, cur)
            if g is not None:
                net = g - rt_bp
                mclips.append(net)
                if net > 0 and spawned < CAP + len(MIMIC4) - 2:
                    new.append(Leg(cur, wide[1], wide[2], wide[3], RECLIP, be_pct, pend))
                    spawned += 1
        extra += new
    last = None
    for i in range(xi, ei, -1):
        if c[i] is not None: last = c[i]; break
    if last is None: last = epx
    if parent.open and not parent.clipped and not parent.dead:
        pclips.append((last / parent.le - 1.0) * 1e4 - rt_bp)
    for lg in mimics + extra:
        if lg.open and not lg.clipped and not lg.dead:
            mclips.append((last / lg.le - 1.0) * 1e4 - rt_bp)
    return pclips, mclips


def metrics(rows):
    if not rows: return dict(n=0, net=0, pf=0, h1=0, h2=0, bull=0, bear=0, worst=0, passed=False)
    net = sum(r[1] for r in rows)
    gw = sum(r[1] for r in rows if r[1] > 0); gl = -sum(r[1] for r in rows if r[1] < 0)
    pf = gw / gl if gl > 0 else (999 if gw > 0 else 0)
    ex = sorted(r[0] for r in rows); mid = ex[len(ex) // 2]
    h1 = sum(r[1] for r in rows if r[0] < mid); h2 = sum(r[1] for r in rows if r[0] >= mid)
    bull = sum(r[1] for r in rows if r[2]); bear = sum(r[1] for r in rows if not r[2])
    worst = min(r[1] for r in rows)
    passed = (len(rows) >= 30 and net > 0 and pf > 1.3 and h1 > 0 and h2 > 0 and bear >= 0)
    return dict(n=len(rows), net=net, pf=pf, h1=h1, h2=h2, bull=bull, bear=bear, worst=worst, passed=passed)


def fmt(tag, m, m2):
    g = "PASS" if m["passed"] else "fail"
    g2 = "2xPASS" if m2["passed"] else "2xfail"
    return (f"{tag:<22} n={m['n']:>6} net={m['net']/100:>+9.0f}% PF={m['pf']:.2f} "
            f"H1={m['h1']/100:+.0f} H2={m['h2']/100:+.0f} bull={m['bull']/100:+.0f} "
            f"bear={m['bear']/100:+.0f} worst={m['worst']/100:+.1f}% {g} {g2}")


def main():
    series, missing = load()
    if missing:
        print(f"WARNING: {len(missing)} wired names missing from feed: {missing}")
    prepped = {s: prep(px) for s, px in series.items()}
    events = {s: parent_trades(c) for s, (c, _) in prepped.items()}
    npar = sum(len(v) for v in events.values())
    print(f"universe {len(prepped)}/45 names, {npar} parent windows (thr {THR*100:.0f}%, entry next close)")
    print(f"parent cell arm{PARENT_CELL[0]:g}/gb{int(PARENT_CELL[2]*100)} lc{LC_PCT:g} | "
          f"mimic4 = {' '.join(f'{t}:a{a:g}/s{s}/gb{int(g*100)}' for t,a,s,g in MIMIC4)}")

    def book(be, pend, rt):
        prow, mrow = [], []
        for s, (c, sma) in prepped.items():
            for (ei, xi) in events[s]:
                bull = sma[ei] is not None and c[ei] is not None and c[ei] > sma[ei]
                pc, mc = run_window(c, ei, xi, be, pend, rt)
                prow += [(ei, x, bull) for x in pc]
                mrow += [(ei, x, bull) for x in mc]
        return prow, mrow

    # parent standalone (identical for every mimic config — print once)
    prow, _ = book(0.5, 3, RT_BP)
    prow2, _ = book(0.5, 3, 2 * RT_BP)
    print("\n== PARENT standalone (the engine, trades on trigger) ==")
    print(fmt("PARENT a1/gb10 lc15", metrics(prow), metrics(prow2)))

    print("\n== 4x MIMIC standalone (BE-entry sweep, judged on its OWN book) ==")
    results = []
    for be in (0.3, 0.5, 1.0, 2.0):
        for pend in (2, 3, 5):
            _, mrow = book(be, pend, RT_BP)
            _, mrow2 = book(be, pend, 2 * RT_BP)
            m, m2 = metrics(mrow), metrics(mrow2)
            results.append((be, pend, m, m2))
            print(fmt(f"MIMIC be{be:g}%/pend{pend}", m, m2))

    winners = [(be, p, m, m2) for be, p, m, m2 in results if m["passed"] and m2["passed"]]
    print(f"\n{len(winners)} config(s) pass all-6 + 2x-cost")
    if winners:
        best = max(winners, key=lambda r: r[2]["net"])
        be, p, m, m2 = best
        print(f"BEST: be{be:g}%/pend{p}  net {m['net']/100:+.0f}%  PF {m['pf']:.2f}  worst {m['worst']/100:+.1f}%")
        # per-leg breakdown at the best cell
        print("\nper-leg at BEST (standalone each):")
        for li, (tag, a, s, g) in enumerate(MIMIC4):
            rows = []
            for sname, (c, sma) in prepped.items():
                for (ei, xi) in events[sname]:
                    bull = sma[ei] is not None and c[ei] > sma[ei]
                    epx = c[ei]
                    lg = Leg(epx, a, s, g, RECLIP, be, p)
                    for i in range(ei, xi):
                        if c[i] is None: continue
                        gg = lg.step(i, c[i])
                        if gg is not None: rows.append((ei, gg - RT_BP, bull))
                    last = None
                    for i in range(xi, ei, -1):
                        if c[i] is not None: last = c[i]; break
                    if last and lg.open and not lg.clipped and not lg.dead:
                        rows.append((ei, (last / lg.le - 1.0) * 1e4 - RT_BP, bull))
            mm = metrics(rows)
            print(fmt(f"  {tag} a{a:g}/s{s}/gb{int(g*100)}", mm, mm))


if __name__ == "__main__":
    main()
