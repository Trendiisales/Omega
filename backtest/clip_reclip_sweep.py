#!/usr/bin/env python3
"""Reclip (RE_TRIG) standalone-clip sweep — does re-arming a fresh companion leg on a
still-running parent (COMPANION_RETRIG_PCT) bank MORE than a single clip, or just churn
re-entry cost away?

Reuses the SYSTEM-OF-RECORD math (standalone_clip_overlay.load_paths/metrics/halves) so
verdicts stay identical to the canonical companion gate. The ONLY addition is a
multi-leg clip walker: after a leg clips, if the parent trade makes a NEW trade-level
favourable high past the prior clip peak by RE_TRIG_PCT, a fresh leg opens AT THE CURRENT
PRICE (its own entry), re-arms on the gate, and clips on its own giveback — paying
cost_rt again each re-entry. retrig=0 collapses to the canonical single-clip book.

Companion is a SEPARATE ADDITIVE engine -> judged STANDALONE all-6 (net>0, PF>1, both WF
halves>0, both regimes>=0), NEVER vs WIDE. See CompanionDominanceError.md.

usage: clip_reclip_sweep.py <path_csv> <LABEL> [gate] [giveback]
"""
import importlib.util, sys, os

HARNESS = os.path.expanduser("~/IBKRCrypto/backtest/standalone_clip_overlay.py")
spec = importlib.util.spec_from_file_location("scl", HARNESS)
scl = importlib.util.module_from_spec(spec); spec.loader.exec_module(scl)


def reclip_legs(t, cfg):
    """Return a LIST of (exit_ms, pnl) legs for the reclip book on one parent trade.
    retrig<=0 -> single leg (matches scl.clip_pnl). Each leg books d*(px/leg_ent-1)-cost."""
    d = t["dir"]; ent = t["ent"]; cost = t["cost"]; path = t["path"]
    gate = cfg["gate"]; rev_frac = cfg.get("rev_frac"); stall = cfg.get("stall")
    rev_atr = (cfg["rev_atr_k"] * t["atr"]) if (cfg.get("rev_atr_k") and t["atr"]) else None
    retrig = cfg.get("retrig", 0.0)
    last = path[-1]

    legs = []
    mode = "active"                 # active leg tracking | clipped waiting to re-arm
    leg_ent = ent; leg_peak = 0.0; leg_since = 0
    trade_peak = 0.0                # trade-level MFE (retrig anchor)
    clip_anchor = None
    leg_last = last                 # last bar seen while a leg is active

    for seq, ms, px in path:
        if seq == 0:
            continue
        fav_trade = d * (px / ent - 1.0)
        if fav_trade > trade_peak:
            trade_peak = fav_trade

        if mode == "active":
            leg_last = (seq, ms, px)
            fav = d * (px / leg_ent - 1.0)
            if fav > leg_peak:
                leg_peak = fav; leg_since = 0
            else:
                leg_since += 1
            if leg_peak < gate:
                continue
            hit = ((stall and leg_since >= stall) or
                   (rev_frac and fav <= leg_peak * (1.0 - rev_frac)) or
                   (rev_atr is not None and fav <= leg_peak - rev_atr))
            if hit:
                legs.append((ms, fav - cost))
                clip_anchor = trade_peak
                if retrig and retrig > 0:
                    mode = "clipped"
                else:
                    mode = "done"
        elif mode == "clipped":
            if clip_anchor is not None and fav_trade > clip_anchor * (1.0 + retrig):
                mode = "active"; leg_ent = px; leg_peak = 0.0; leg_since = 0
                leg_last = (seq, ms, px)

    # a leg still active at parent flip: companion holds to the same flip -> book at last bar
    if mode == "active":
        _, ms, px = leg_last
        legs.append((ms, d * (px / leg_ent - 1.0) - cost))
    return legs


def run(csvf, label, gate, gv):
    trades = scl.load_paths(csvf)
    print(f"\n=== {label}  ({len(trades)} trades)  gate={gate*100:.2f}%  giveback={gv*100:.0f}% ===")
    print(f"  {'retrig':>7s} {'legs':>5s} {'net%':>8s} {'PF':>5s} {'maxDD%':>8s} {'MAR':>6s} "
          f"{'WF-H1':>7s} {'WF-H2':>7s} {'bull':>7s} {'bear':>7s}  verdict")
    for retrig in [0.0, 0.02, 0.05, 0.10, 0.20]:
        cfg = dict(gate=gate, stall=None, rev_frac=gv, rev_atr_k=None, retrig=retrig)
        reg = []   # (ms, pnl, bull)
        for t in trades:
            for ms, pnl in reclip_legs(t, cfg):
                reg.append((ms, pnl, t["bull"]))
        m = scl.metrics([(x[0], x[1]) for x in reg])
        bull = scl.metrics([(x[0], x[1]) for x in reg if x[2]])
        bear = scl.metrics([(x[0], x[1]) for x in reg if not x[2]])
        h1, h2 = scl.halves([(x[0], x[1]) for x in reg])
        m1 = scl.metrics(h1); m2 = scl.metrics(h2)
        ok = (m["net"] > 0 and m["pf"] > 1 and m1["net"] > 0 and m2["net"] > 0
              and bull["net"] >= 0 and bear["net"] >= 0)
        print(f"  {retrig*100:6.0f}% {len(reg):5d} {m['net']:8.0f} {m['pf']:5.2f} {m['dd']:8.0f} "
              f"{m['mar']:6.2f} {m1['net']:7.0f} {m2['net']:7.0f} {bull['net']:7.0f} {bear['net']:7.0f}  "
              f"{'PASS' if ok else '.'}")


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("usage: clip_reclip_sweep.py <path_csv> <LABEL> [gate] [giveback]"); sys.exit(1)
    gate = float(sys.argv[3]) if len(sys.argv) > 3 else 0.01
    gv = float(sys.argv[4]) if len(sys.argv) > 4 else 0.50
    run(sys.argv[1], sys.argv[2], gate, gv)
