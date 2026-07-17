#!/usr/bin/env python3
"""Certify IBS be-mode reclip GAP-25 variant (operator 2026-07-17: 'capture every
25 instead of 50 after the initial clip').

Current shipped semantics : reopen when fav >= peak + retrig + confirm ; anchor = peak + retrig
GAP-25 variant            : reopen when fav >= peak + retrig            ; anchor = peak + retrig - confirm
Invariant preserved: anchor = reopen_level - confirm (leg opens confirm above its
anchor, identical BE-entry room; floor = anchor + cost). With retrig=25/confirm=25
the reopen gap past prior peak drops 50 -> 25 and the reclip anchor sits AT the
prior peak, so the never-banked strip between clips shrinks from $50 to $25.
No overlap with prior banked segment (prior bank level = peak - trail <= peak = new anchor).
"""
import sys, importlib.util
spec = importlib.util.spec_from_file_location(
    "base", "/Users/jo/Omega/backtest/stall_clip_sweep_idx_bearshort.py")
base = importlib.util.module_from_spec(spec)
sys.modules["base"] = base
try:
    _argv = sys.argv; sys.argv = [_argv[0]]   # keep base's __main__ guard quiet
    spec.loader.exec_module(base)
finally:
    sys.argv = _argv

def sim_leg_floored_gap25(rows, confirm_usd, trail_usd, retrig_usd=25.0, cost_mult=1.0):
    entry = rows[0][2]; d = rows[0][1]
    cost_usd = rows[0][5] * entry * cost_mult
    banks = []
    open_pos = False; mfe = 0.0; last_upnl = 0.0
    clipped = False; peak = 0.0; anchor = 0.0
    for (_, _, _, px, _, _) in rows:
        upnl = d * (px - entry)
        if clipped:
            # GAP-25: reopen when upnl clears peak + retrig; anchor = reopen - confirm
            if retrig_usd > 0.0 and peak > 0.0 and upnl >= peak + retrig_usd:
                clipped = False; anchor = peak + retrig_usd - confirm_usd
                open_pos = True; mfe = upnl
            else:
                continue
        if not open_pos:
            if upnl < confirm_usd: continue
            open_pos = True; mfe = upnl
        if upnl > mfe: mfe = upnl
        last_upnl = upnl
        floor_lvl = anchor + cost_usd
        stop_lvl = max(mfe - trail_usd, floor_lvl)
        if upnl <= stop_lvl:
            banks.append((upnl - anchor - cost_usd, "FLOOR_CLIP")); open_pos = False
            clipped = True; peak = mfe; continue
    if open_pos:
        banks.append((last_upnl - anchor - cost_usd, "ENGINE_EXIT"))
    return banks

def run(legs, sim, cnf, tr, retrig, cm):
    all_banks = []
    for rows in legs:
        bs = sim(rows, cnf, tr, retrig, cm)
        all_banks.extend(b[0] for b in bs)
    return all_banks

def report(path, label):
    legs = base.load(path)
    h1 = legs[:len(legs)//2]
    h2 = legs[len(legs)//2:]
    print(f"== {label} ==  legs={len(legs)}")
    print(f"{'variant':10s} {'cell':22s} {'n':>4s} {'net$':>9s} {'PF':>7s} {'worst':>8s} {'WF-H1':>8s} {'WF-H2':>8s}")
    for name, sim in (("SHIPPED", base.sim_leg_floored), ("GAP25", sim_leg_floored_gap25)):
        for cm, cmtag in ((1.0, "x1"), (2.0, "x2")):
            banks = run(legs, sim, 25.0, 25.0, 25.0, cm)
            b1 = run(h1, sim, 25.0, 25.0, 25.0, cm)
            b2 = run(h2, sim, 25.0, 25.0, 25.0, cm)
            n = len(banks); net = sum(banks)
            gp = sum(x for x in banks if x > 0); gl = -sum(x for x in banks if x < 0)
            pf = gp/gl if gl > 1e-9 else 999.0
            worst = min(banks) if banks else 0.0
            print(f"{name:10s} cnf25/tr25/rt25 cost{cmtag:3s} {n:4d} {net:9.1f} {pf:7.2f} {worst:8.1f} {sum(b1):8.1f} {sum(b2):8.1f}")
    # grid sanity for GAP25 x1: confirm x trail
    print(f"-- GAP25 grid cost_x1 --")
    for cnf in (10.0, 25.0, 50.0):
        for tr in (25.0, 50.0, 75.0, 100.0):
            banks = run(legs, sim_leg_floored_gap25, cnf, tr, 25.0, 1.0)
            b1 = run(h1, sim_leg_floored_gap25, cnf, tr, 25.0, 1.0)
            b2 = run(h2, sim_leg_floored_gap25, cnf, tr, 25.0, 1.0)
            n = len(banks); net = sum(banks)
            gp = sum(x for x in banks if x > 0); gl = -sum(x for x in banks if x < 0)
            pf = gp/gl if gl > 1e-9 else 999.0
            worst = min(banks) if banks else 0.0
            v = "PASS" if (net > 0 and pf >= 1.3 and sum(b1) > 0 and sum(b2) > 0) else "fail"
            print(f"  cnf{cnf:.0f}/tr{tr:.0f} | n={n:4d} net={net:9.1f} PF={pf:7.2f} worst={worst:8.1f} WF={sum(b1):8.1f}/{sum(b2):8.1f} {v}")

if __name__ == "__main__":
    report(sys.argv[1], sys.argv[2])
