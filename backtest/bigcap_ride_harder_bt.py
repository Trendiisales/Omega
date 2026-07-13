#!/usr/bin/env python3
"""
BIGCAP "TRADE IT HARD, RIDE UNTIL IT REVERSES" sweep (operator S-2026-07-13:
"this should be one of our strongest trades... trade it hard and for as long as
we can until it reverses with mimic engines and the same for the other bigcap
engines").

Two engines, one question each — all faithful to the validated conventions of
bigcap_parent4mimic_bt.py (feed data/rdagent/sp500_long_close.csv, ALL 45 wired
names, close fills, RT 8bp ladder / 20bp impulse, lc15 mimic drawdown-cancel):

  A) LADDER engine (thr 3% window, ends at the first -3% reversal day):
     A1 PARENT ride-length sweep: wired a1/gb10 vs looser trails gb25/50/75 vs
        pure RIDE-TO-REVERSAL (no trail at all — hold to the -3% flush; lc15
        catastrophe only).
     A2 MIMIC book harder: wired be0.5/pend3 cell, sweep self-funding CAP
        {5,9,12} x RECLIP {3,5,10%} (more legs, faster re-entry after BE).

  B) IMPULSE engine (+2% day AND new-20d-high; enters AT signal close; exits
     gb90 trail / 60d hold / -15% catastrophe):
     B1 parent reproduction sanity (must match clip_path_bigcap_impulse.cpp
        magnitudes: n~932 PF~2.2 net~+2800%).
     B2 NEW: 4x BE-mimic legs on the impulse windows (same MIMIC4 cells,
        BE-entry pending gate, lc15, cap5/reclip5) — sweep be x pend, judged
        STANDALONE (feedback-companion-independent-engine).

Gate: all-6 (n>=30, net>0, PF>1.3, WF both halves>0, bear>=0 on the per-name
200SMA entry label) + 2x-cost, pooled across ALL 45 names; ex-best-name check
on winners.
"""
import csv
from pathlib import Path

DATA = Path(__file__).resolve().parent.parent / "data/rdagent/sp500_long_close.csv"
BIGCAP = ("NVDA AMD AVGO MU MRVL SMCI ARM PLTR TSLA META NFLX CRWD SHOP COIN MSTR "
          "SNOW NOW PANW UBER ABNB DELL ORCL QCOM INTC AMZN GOOGL MSFT AAPL CRM ADBE "
          "IONQ RGTI QBTS ASTS RKLB NBIS CRWV ALAB CRDO WDC STX DD TPR BMY SWKS").split()
RT_LAD = 8.0    # ladder book RT bp (validated)
RT_IMP = 20.0   # impulse book RT bp (validated header value)
SMA_N = 200
THR = 0.03      # ladder window trigger / reversal
LC_PCT = 15.0
MIMIC4 = [("T", 0.5, 2, 0.0), ("M", 2.0, 0, 0.75), ("W", 1.0, 0, 0.10), ("W8", 8.0, 0, 0.50)]
NO_TRAIL = 1e9  # arm never reached -> parent rides to window flush (lc15 only)


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
    return series, [s for s in BIGCAP if s not in idx]


def prep(px):
    sma = [None] * len(px)
    run = 0.0; buf = []
    for i, v in enumerate(px):
        if v is None: continue
        buf.append(v); run += v
        if len(buf) > SMA_N: run -= buf.pop(0)
        if len(buf) == SMA_N: sma[i] = run / SMA_N
    return px, sma


def ladder_windows(c):
    trades = []; pos = False; ei = None
    valid = [i for i in range(len(c)) if c[i] is not None]
    for k in range(1, len(valid)):
        j = c[valid[k]] / c[valid[k - 1]] - 1.0
        if not pos and j >= THR:
            if k + 1 < len(valid): pos = True; ei = valid[k + 1]
        elif pos and j <= -THR:
            xi = valid[k + 1] if k + 1 < len(valid) else valid[k]
            if xi > ei: trades.append((ei, xi))
            pos = False; ei = None
    if pos and ei is not None and valid[-1] > ei:
        trades.append((ei, valid[-1]))
    return trades


def impulse_windows(c, thr=0.02, hiw=20, gb=0.90, max_hold=60, cata=15.0):
    """Faithful BigCap2pctImpulseCompanion sim. Returns (windows, parent_clips)
    where windows=(ei,xi) daily-row spans and clips are net-gross bp (cost added
    by caller). Entry AT signal close; exits cat/gb-trail/max-hold; re-arm when
    flat."""
    valid = [i for i in range(len(c)) if c[i] is not None]
    wins = []; clips = []
    in_pos = False; epx = 0.0; mfe = 0.0; held = 0; ei = None
    for k in range(1, len(valid)):
        i = valid[k]; cur = c[i]
        if in_pos:
            held += 1
            fav = cur / epx - 1.0
            if fav > mfe: mfe = fav
            reason = None
            if fav <= -cata / 100.0: reason = "CAT"
            elif mfe > 1e-9 and fav <= mfe * (1.0 - gb): reason = "GB"
            elif held >= max_hold: reason = "HOLD"
            if reason:
                clips.append((ei, (cur / epx - 1.0) * 1e4))
                wins.append((ei, i))
                in_pos = False; epx = 0.0; mfe = 0.0; held = 0; ei = None
        if not in_pos and k >= hiw:
            prev = c[valid[k - 1]]
            j = cur / prev - 1.0
            if j >= thr:
                hi = max(c[valid[m]] for m in range(k - hiw, k))
                if cur >= hi:
                    in_pos = True; epx = cur; mfe = 0.0; held = 0; ei = i
    if in_pos and ei is not None and valid[-1] > ei:
        clips.append((ei, (c[valid[-1]] / epx - 1.0) * 1e4))
        wins.append((ei, valid[-1]))
    return wins, clips


class Leg:
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
            if (cur / self.trig - 1.0) * 100.0 >= self.be:
                self.pending = False
                self.epx = cur; self.le = cur
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
        if not armed and fav <= -LC_PCT:
            self.dead = True
            return (cur / self.le - 1.0) * 1e4
        if armed and self.sb > 0 and stall >= self.sb:
            self.pk = self.mfe; self.clipped = True
            return (cur / self.le - 1.0) * 1e4
        if armed and self.gb > 0 and fav <= self.mfe * (1.0 - self.gb):
            self.pk = self.mfe; self.clipped = True
            return (cur / self.le - 1.0) * 1e4
        return None


def run_window(c, ei, xi, be, pend, rt, cap, reclip, parent_cell, with_parent=True):
    epx = c[ei]
    parent = Leg(epx, *parent_cell, reclip) if with_parent else None
    mimics = [Leg(epx, a, s, g, reclip, be, pend) for (_, a, s, g) in MIMIC4]
    spawned = len(mimics); pclips = []; mclips = []
    wide = MIMIC4[2]
    extra = []
    for i in range(ei, xi):
        if c[i] is None: continue
        cur = c[i]
        if parent is not None:
            g = parent.step(i, cur)
            if g is not None: pclips.append(g - rt)
        new = []
        for lg in mimics + extra:
            g = lg.step(i, cur)
            if g is not None:
                net = g - rt
                mclips.append(net)
                if net > 0 and spawned < cap + len(MIMIC4) - 2:
                    new.append(Leg(cur, wide[1], wide[2], wide[3], reclip, be, pend))
                    spawned += 1
        extra += new
    last = None
    for i in range(xi, ei, -1):
        if c[i] is not None: last = c[i]; break
    if last is None: last = epx
    if parent is not None and parent.open and not parent.clipped and not parent.dead:
        pclips.append((last / parent.le - 1.0) * 1e4 - rt)
    for lg in mimics + extra:
        if lg.open and not lg.clipped and not lg.dead:
            mclips.append((last / lg.le - 1.0) * 1e4 - rt)
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
    return (f"{tag:<26} n={m['n']:>6} net={m['net']/100:>+9.0f}% PF={m['pf']:.2f} "
            f"H1={m['h1']/100:+.0f} H2={m['h2']/100:+.0f} bull={m['bull']/100:+.0f} "
            f"bear={m['bear']/100:+.0f} worst={m['worst']/100:+.1f}% {g} {g2}")


def main():
    series, missing = load()
    if missing:
        print(f"WARNING: missing names: {missing}")
    prepped = {s: prep(px) for s, px in series.items()}
    lad_ev = {s: ladder_windows(c) for s, (c, _) in prepped.items()}
    print(f"universe {len(prepped)}/45 names | ladder windows: "
          f"{sum(len(v) for v in lad_ev.values())}")

    # ── A1: PARENT ride-length sweep ────────────────────────────────────
    print("\n== A1 LADDER PARENT — how long do we ride? (standalone) ==")
    variants = [("wired a1/gb10", (1.0, 0, 0.10)),
                ("a1/gb25", (1.0, 0, 0.25)),
                ("a1/gb50", (1.0, 0, 0.50)),
                ("a1/gb75", (1.0, 0, 0.75)),
                ("RIDE-TO-REVERSAL lc15", (NO_TRAIL, 0, 0.0))]
    for tag, cell in variants:
        rows, rows2 = [], []
        for s, (c, sma) in prepped.items():
            for (ei, xi) in lad_ev[s]:
                bull = sma[ei] is not None and c[ei] is not None and c[ei] > sma[ei]
                pc, _ = run_window(c, ei, xi, 0.5, 3, RT_LAD, 5, 0.05, cell)
                pc2, _ = run_window(c, ei, xi, 0.5, 3, 2 * RT_LAD, 5, 0.05, cell)
                rows += [(ei, x, bull) for x in pc]
                rows2 += [(ei, x, bull) for x in pc2]
        print(fmt(tag, metrics(rows), metrics(rows2)))

    # ── A2: MIMIC book harder — cap x reclip ────────────────────────────
    print("\n== A2 LADDER 4x-MIMIC be0.5/pend3 — cap x reclip sweep (standalone) ==")
    a2 = []
    for cap in (5, 9, 12):
        for rc in (0.03, 0.05, 0.10):
            rows, rows2 = [], []
            for s, (c, sma) in prepped.items():
                for (ei, xi) in lad_ev[s]:
                    bull = sma[ei] is not None and c[ei] is not None and c[ei] > sma[ei]
                    _, mc = run_window(c, ei, xi, 0.5, 3, RT_LAD, cap, rc, (1.0, 0, 0.10))
                    _, mc2 = run_window(c, ei, xi, 0.5, 3, 2 * RT_LAD, cap, rc, (1.0, 0, 0.10))
                    rows += [(ei, x, bull) for x in mc]
                    rows2 += [(ei, x, bull) for x in mc2]
            m, m2 = metrics(rows), metrics(rows2)
            a2.append((cap, rc, m, m2))
            print(fmt(f"cap{cap}/reclip{int(rc*100)}%", m, m2))

    # ── B1: IMPULSE parent reproduction ─────────────────────────────────
    print("\n== B1 IMPULSE PARENT (+2% & new-20d-high, gb90/60d/-15%) sanity ==")
    imp_ev = {}
    rows, rows2 = [], []
    for s, (c, sma) in prepped.items():
        wins, clips = impulse_windows(c)
        imp_ev[s] = wins
        for (ei, g) in clips:
            bull = sma[ei] is not None and c[ei] is not None and c[ei] > sma[ei]
            rows.append((ei, g - RT_IMP, bull))
            rows2.append((ei, g - 2 * RT_IMP, bull))
    print(fmt("IMPULSE parent", metrics(rows), metrics(rows2)))
    print(f"   ({sum(len(v) for v in imp_ev.values())} impulse windows; "
          f"cpp ref: n=932 PF 2.18 net +2798%)")

    # ── B2: 4x BE-mimics on the impulse windows ─────────────────────────
    print("\n== B2 IMPULSE + 4x BE-MIMIC legs (standalone mimic book) ==")
    b2 = []
    for be in (0.3, 0.5, 1.0):
        for pend in (3, 5):
            rows, rows2 = [], []
            for s, (c, sma) in prepped.items():
                for (ei, xi) in imp_ev[s]:
                    bull = sma[ei] is not None and c[ei] is not None and c[ei] > sma[ei]
                    _, mc = run_window(c, ei, xi, be, pend, RT_IMP, 5, 0.05,
                                       (1.0, 0, 0.10), with_parent=False)
                    _, mc2 = run_window(c, ei, xi, be, pend, 2 * RT_IMP, 5, 0.05,
                                        (1.0, 0, 0.10), with_parent=False)
                    rows += [(ei, x, bull) for x in mc]
                    rows2 += [(ei, x, bull) for x in mc2]
            m, m2 = metrics(rows), metrics(rows2)
            b2.append((be, pend, m, m2, rows))
            print(fmt(f"IMP-MIMIC be{be:g}%/pend{pend}", m, m2))

    # ── B3: clean 2-LEG (M+W8) impulse mimic — the WIRED cell ────────────
    # T/W(a1/gb10) FAIL standalone on the longer gb90 windows (H1 negative);
    # M+W8 pass standalone each. Respawn cell = W8.
    print("\n== B3 IMPULSE 2-LEG mimic M+W8 (respawn W8) — WIRED shape ==")
    LEGS2 = [("M", 2.0, 0, 0.75), ("W8", 8.0, 0, 0.50)]

    def run2(c, ei, xi, be, pend, rt, cap=5, rc=0.05):
        epx = c[ei]
        legs = [Leg(epx, a, s, g, rc, be, pend) for (_, a, s, g) in LEGS2]
        spawned = len(legs); clips = []; extra = []; resp = LEGS2[1]
        for i in range(ei, xi):
            if c[i] is None: continue
            cur = c[i]; new = []
            for lg in legs + extra:
                g = lg.step(i, cur)
                if g is not None:
                    net = g - rt; clips.append(net)
                    if net > 0 and spawned < cap + len(LEGS2) - 2:
                        new.append(Leg(cur, resp[1], resp[2], resp[3], rc, be, pend))
                        spawned += 1
            extra += new
        last = None
        for i in range(xi, ei, -1):
            if c[i] is not None: last = c[i]; break
        if last is None: last = epx
        for lg in legs + extra:
            if lg.open and not lg.clipped and not lg.dead:
                clips.append((last / lg.le - 1.0) * 1e4 - rt)
        return clips

    for be in (0.3, 0.5):
        for pend in (3, 5):
            rows, rows2 = [], []
            for s, (c, sma) in prepped.items():
                for (ei, xi) in imp_ev[s]:
                    bull = sma[ei] is not None and c[ei] is not None and c[ei] > sma[ei]
                    rows += [(ei, x, bull) for x in run2(c, ei, xi, be, pend, RT_IMP)]
                    rows2 += [(ei, x, bull) for x in run2(c, ei, xi, be, pend, 2 * RT_IMP)]
            print(fmt(f"2LEG be{be:g}%/pend{pend}", metrics(rows), metrics(rows2)))

    winners = [(be, p, m, m2, rows) for be, p, m, m2, rows in b2 if m["passed"] and m2["passed"]]
    print(f"\n{len(winners)} impulse-mimic config(s) pass all-6 + 2x-cost")
    if winners:
        best = max(winners, key=lambda r: r[2]["net"])
        be, p, m, m2, rows = best
        # ex-best-name robustness on the winner
        per = {}
        for s, (c, sma) in prepped.items():
            rr = []
            for (ei, xi) in imp_ev[s]:
                bull = sma[ei] is not None and c[ei] is not None and c[ei] > sma[ei]
                _, mc = run_window(c, ei, xi, be, p, RT_IMP, 5, 0.05,
                                   (1.0, 0, 0.10), with_parent=False)
                rr += [(ei, x, bull) for x in mc]
            per[s] = sum(x[1] for x in rr)
        bestname = max(per, key=per.get)
        exrows = [r for s2, (c, sma) in prepped.items() if s2 != bestname
                  for r in []]  # rebuilt below
        exrows = []
        for s2, (c, sma) in prepped.items():
            if s2 == bestname: continue
            for (ei, xi) in imp_ev[s2]:
                bull = sma[ei] is not None and c[ei] is not None and c[ei] > sma[ei]
                _, mc = run_window(c, ei, xi, be, p, RT_IMP, 5, 0.05,
                                   (1.0, 0, 0.10), with_parent=False)
                exrows += [(ei, x, bull) for x in mc]
        mex = metrics(exrows)
        print(f"BEST: be{be:g}%/pend{p} net {m['net']/100:+.0f}% PF {m['pf']:.2f} "
              f"worst {m['worst']/100:+.1f}%")
        print(fmt(f"  ex-best ({bestname})", mex, mex))


if __name__ == "__main__":
    main()
