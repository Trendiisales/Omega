#!/usr/bin/env python3
"""Giveback-ladder standalone-clip sweep. Reuses the SYSTEM-OF-RECORD harness math
(standalone_clip_overlay.clip_pnl/metrics/halves/load_paths) so verdicts are identical
to the canonical companion gate. Sweeps REVERSAL_GIVEBACK (rev_frac) across a ladder,
each judged STANDALONE all-6 (net>0, PF>1, both WF halves>0, both regimes>=0) — never
vs WIDE (companion is an independent additive book).

usage: clip_giveback_ladder.py <path_csv> <LABEL> [arm_gate]
"""
import importlib.util, sys, os

HARNESS = os.path.expanduser("~/IBKRCrypto/backtest/standalone_clip_overlay.py")
spec = importlib.util.spec_from_file_location("scl", HARNESS)
scl = importlib.util.module_from_spec(spec); spec.loader.exec_module(scl)

def run(csvf, label, arm_gate):
    trades = scl.load_paths(csvf)
    ladder = [0.20, 0.30, 0.50, 0.60, 0.70, 0.80, 0.90]
    print(f"\n=== {label}  ({len(trades)} trades)  arm_gate={arm_gate*100:.2f}% ===")
    print(f"  {'giveback':>9s} {'net%':>8s} {'PF':>5s} {'maxDD%':>8s} {'MAR':>6s} "
          f"{'WF-H1':>7s} {'WF-H2':>7s} {'bull':>7s} {'bear':>7s}  n  verdict")
    for gv in ladder:
        cfg = dict(gate=arm_gate, stall=None, rev_frac=gv, rev_atr_k=None)
        reg = [(scl.clip_pnl(t, cfg)[0], scl.clip_pnl(t, cfg)[1], t["bull"]) for t in trades]
        m = scl.metrics([(x[0], x[1]) for x in reg])
        bull = scl.metrics([(x[0], x[1]) for x in reg if x[2]])
        bear = scl.metrics([(x[0], x[1]) for x in reg if not x[2]])
        h1, h2 = scl.halves([(x[0], x[1]) for x in reg])
        m1, m2 = scl.metrics(h1), scl.metrics(h2)
        ok = (m["net"] > 0 and m["pf"] > 1 and m1["net"] > 0 and m2["net"] > 0
              and bull["net"] >= 0 and bear["net"] >= 0)
        tag = "existing" if gv in (0.30, 0.50) else "NEW"
        print(f"  {gv:8.0%}  {m['net']:8.0f} {m['pf']:5.2f} {m['dd']:8.0f} {m['mar']:6.2f} "
              f"{m1['net']:7.0f} {m2['net']:7.0f} {bull['net']:7.0f} {bear['net']:7.0f} "
              f"{m['n']:3d}  {'PASS' if ok else '.':4s} [{tag}]")

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("usage: clip_giveback_ladder.py <path_csv> <LABEL> [arm_gate]"); sys.exit(1)
    run(sys.argv[1], sys.argv[2], float(sys.argv[3]) if len(sys.argv) > 3 else 0.05)
