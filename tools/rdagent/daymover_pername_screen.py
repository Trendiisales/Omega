#!/usr/bin/env python3
"""
DAY-MOVER BE-FLOOR x3 — PER-NAME VIABILITY SCREEN (S-2026-07-06).

Operator: "if we have 10 stocks that are viable (more or less) trade ALL of them
with our BE engines x3." -> select the LIVE universe per-name, not a hardcoded list.

A name is VIABLE (long and/or short flavor) if, on its OWN BE-floor book:
  - gross neg == 0      (by construction; assert it held)
  - net_tot > 0         (after 10bp RT cost)
  - both WF halves > 0  (h1>0 and h2>0)      [robustness]
  - gate tier = r150 (the robust > daily-noise runner; r20 is daily-coarse)
Both regimes (bull/bear) reported but NOT required (some names lack bear samples).

Mechanism identical to daymover_befloor_x3_v2.py (the LIVE BeFloor family).
Output: a ranked table + a JSON universe file the C++ companion seeds from.
"""
from __future__ import annotations
import json
import numpy as np, pandas as pd
from pathlib import Path

CSV      = Path.home() / "Omega" / "data" / "rdagent" / "sp500_long_close.csv"
OUT      = Path.home() / "Omega" / "data" / "rdagent" / "daymover_universe.json"
# MUST == engine_init BIGCAP_LAD (all 45). Enforced EXACT by scripts/roster_parity_audit.py
# check [4] -- any add/drop that misses this copy fails the mac canary. S-2026-07-17: 39->45
# (added WDC STX DD TPR BMY SWKS) to match the ladder roster; flipped from SUBSET to EXACT.
BIGCAP   = ("NVDA AMD AVGO MU MRVL SMCI ARM PLTR TSLA META NFLX CRWD SHOP COIN MSTR "
            "SNOW NOW PANW UBER ABNB DELL ORCL QCOM INTC AMZN GOOGL MSFT AAPL CRM ADBE "
            "IONQ RGTI QBTS ASTS RKLB NBIS CRWV ALAB CRDO WDC STX DD TPR BMY SWKS").split()
GLITCH   = {"POM", "CPWR", "MI"}
W        = 1
BE_BP    = 10.0
COST_BP  = 10.0
GATE_GB  = 150.0     # r150 runner = the robust gate tier for viability
THR      = 0.03
MIN_COV  = 300


def load():
    df = pd.read_csv(CSV, index_col=0); df.index = pd.to_datetime(df.index); return df


def windows(c, thr, up):
    tr = []; N = len(c); pos = False; ent = 0
    for i in range(W, N):
        if not np.isfinite(c[i]) or not np.isfinite(c[i - W]): continue
        j = c[i] / c[i - W] - 1.0
        enter = (j >= thr) if up else (j <= -thr)
        exit_ = (j <= -thr) if up else (j >= thr)
        if not pos and enter:
            ei = i + 1
            if ei >= N: continue
            pos = True; ent = ei
        elif pos and exit_:
            xi = i + 1
            if xi >= N: xi = N - 1
            tr.append((ent, xi)); pos = False
    if pos: tr.append((ent, N - 1))
    return tr


def leg_book(c, trades, gb, is_long):
    rows = []
    for ei, xi in trades:
        if not np.isfinite(c[ei]): continue
        ref = c[ei]; entry = None; wm = None
        for i in range(ei, xi):
            cur = c[i]
            if not np.isfinite(cur): continue
            if entry is None:
                cond = ((cur/ref-1.0)*1e4 >= BE_BP) if is_long else ((1.0-cur/ref)*1e4 >= BE_BP)
                if cond: entry = cur; wm = cur
                continue
            if is_long:
                if cur > wm: wm = cur
                stop = max(entry, wm*(1.0-gb/1e4))
                if cur <= stop: rows.append((i, stop/entry-1.0)); ref = stop; entry = None; wm = None
            else:
                if cur < wm: wm = cur
                stop = min(entry, wm*(1.0+gb/1e4))
                if cur >= stop: rows.append((i, entry/stop-1.0)); ref = stop; entry = None; wm = None
        if entry is not None:
            last = c[xi-1] if (xi-1 >= ei and np.isfinite(c[xi-1])) else c[ei]
            g = (last/entry-1.0) if is_long else (entry/last-1.0)
            rows.append((xi, max(0.0, g)))
    return rows


def name_metrics(c, dates, is_long):
    tr = windows(c, THR, is_long)
    rows = leg_book(c, tr, GATE_GB, is_long)
    if not rows: return None
    gross = np.array([r[1] for r in rows]); net = gross - COST_BP/1e4
    half = dates[len(dates)//2]; xd = np.array([dates[r[0]] for r in rows])
    h1 = net[xd < half]; h2 = net[xd >= half]
    return dict(n=len(rows), gneg=int((gross < -1e-9).sum()),
                net_tot=net.sum()*100, net_avg_bp=net.mean()*1e4,
                wr=float((net > 0).mean()),
                h1=h1.sum()*100 if len(h1) else 0.0,
                h2=h2.sum()*100 if len(h2) else 0.0)


def viable(m):
    return m is not None and m['gneg'] == 0 and m['net_tot'] > 0 and m['h1'] > 0 and m['h2'] > 0


def main():
    df = load(); dates = df.index.values
    names = [c for c in df.columns if c not in GLITCH and np.isfinite(df[c].values.astype(float)).sum() >= MIN_COV]
    print(f"PER-NAME SCREEN | {df.index[0].date()}->{df.index[-1].date()} | {len(names)} names >= {MIN_COV}d cov")
    print(f"gate: thr={THR*100:.0f}% r{GATE_GB:.0f} runner, be={BE_BP:.0f}bp cost={COST_BP:.0f}bp | DAILY-close grade")
    print(f"VIABLE = gross-neg==0 AND net>0 AND both WF halves>0\n")

    universe = {"long": [], "short": []}
    rank = []
    for nm in names:
        c = df[nm].values.astype(float)
        mL = name_metrics(c, dates, True); mS = name_metrics(c, dates, False)
        vL = viable(mL); vS = viable(mS)
        if vL: universe["long"].append(nm)
        if vS: universe["short"].append(nm)
        if vL or vS:
            rank.append((nm, mL, mS, vL, vS))

    rank.sort(key=lambda r: (r[1]['net_tot'] if r[1] else 0) + (r[2]['net_tot'] if r[2] else 0), reverse=True)
    hdr = f"  {'name':6s} {'L?':2s} {'Lnet%':>8s} {'Lclip':>5s} {'LWR':>4s} | {'S?':2s} {'Snet%':>8s} {'Sclip':>5s} {'SWR':>4s}"
    print(hdr)
    for nm, mL, mS, vL, vS in rank:
        lp = f"{mL['net_tot']:+8.1f} {mL['n']:5d} {mL['wr']*100:3.0f}%" if mL else f"{'-':>8s} {'-':>5s} {'-':>4s}"
        sp = f"{mS['net_tot']:+8.1f} {mS['n']:5d} {mS['wr']*100:3.0f}%" if mS else f"{'-':>8s} {'-':>5s} {'-':>4s}"
        print(f"  {nm:6s} {'Y' if vL else '.':2s} {lp} | {'Y' if vS else '.':2s} {sp}")

    bigcap_L = [n for n in universe['long'] if n in BIGCAP]
    print(f"\nVIABLE LONG : {len(universe['long'])} names ({len(bigcap_L)} of them bigcap)")
    print(f"VIABLE SHORT: {len(universe['short'])} names")
    print(f"bigcap viable-long: {' '.join(bigcap_L)}")

    OUT.write_text(json.dumps({
        "generated": "S-2026-07-06", "gate": {"thr": THR, "gb": GATE_GB, "be_bp": BE_BP, "cost_bp": COST_BP},
        "grade": "daily-close", "long": sorted(universe['long']), "short": sorted(universe['short']),
        "bigcap_long": sorted(bigcap_L),
    }, indent=2))
    print(f"\n-> wrote {OUT}")


if __name__ == "__main__": main()
