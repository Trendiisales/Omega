#!/usr/bin/env python3
"""Operator S-2026-07-14: 'if the daily shows more than 2% we should trade that
stock' — test DROPPING the new-20d-high condition from BigCap2pctImpulseCompanion
(hiw=1 makes the high-check degenerate: +2% close always >= prior close).
Compares WIRED (+2% & 20d-high) vs 2%-ONLY, parent + the LIVE 2-leg mimic cell
(M a2/gb75 + W8 a8/gb50, be0.5/pend5, cap5/reclip5, lc15). Gate: all-6 + 2x cost.
Faithful reuse of backtest/bigcap_ride_harder_bt.py machinery.
"""
import importlib.util, sys
spec = importlib.util.spec_from_file_location(
    "brh", "/Users/jo/Omega/backtest/bigcap_ride_harder_bt.py")
brh = importlib.util.module_from_spec(spec)
spec.loader.exec_module(brh)

Leg, metrics, fmt = brh.Leg, brh.metrics, brh.fmt
RT_IMP = brh.RT_IMP
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

series, missing = brh.load()
if missing: print("WARNING missing:", missing)
prepped = {s: brh.prep(px) for s, px in series.items()}

for tag, hiw in (("WIRED +2% & 20d-high", 20), ("2%-ONLY (no high gate)", 1)):
    imp_ev = {}
    prow, prow2 = [], []
    for s, (c, sma) in prepped.items():
        wins, clips = brh.impulse_windows(c, thr=0.02, hiw=hiw)
        imp_ev[s] = wins
        for (ei, g) in clips:
            bull = sma[ei] is not None and c[ei] is not None and c[ei] > sma[ei]
            prow.append((ei, g - RT_IMP, bull))
            prow2.append((ei, g - 2 * RT_IMP, bull))
    print(f"\n== {tag} ==  windows={sum(len(v) for v in imp_ev.values())}")
    print(brh.fmt("  PARENT gb90/60d/-15%", metrics(prow), metrics(prow2)))

    mrow, mrow2 = [], []
    per = {}
    for s, (c, sma) in prepped.items():
        rr = []
        for (ei, xi) in imp_ev[s]:
            bull = sma[ei] is not None and c[ei] is not None and c[ei] > sma[ei]
            mc = run2(c, ei, xi, 0.5, 5, RT_IMP)
            mc2 = run2(c, ei, xi, 0.5, 5, 2 * RT_IMP)
            rr += [(ei, x, bull) for x in mc]
            mrow += [(ei, x, bull) for x in mc]
            mrow2 += [(ei, x, bull) for x in mc2]
        per[s] = sum(x[1] for x in rr)
    m = metrics(mrow)
    print(brh.fmt("  MIMIC 2-leg be0.5/pend5", m, metrics(mrow2)))
    best = max(per, key=per.get)
    exrow = [r for r in mrow]  # rebuild ex-best
    exrow = []
    for s, (c, sma) in prepped.items():
        if s == best: continue
        for (ei, xi) in imp_ev[s]:
            bull = sma[ei] is not None and c[ei] is not None and c[ei] > sma[ei]
            exrow += [(ei, x, bull) for x in run2(c, ei, xi, 0.5, 5, RT_IMP)]
    mex = metrics(exrow)
    print(brh.fmt(f"  MIMIC ex-best ({best})", mex, mex))
