#!/usr/bin/env python3
"""stall_clip_sweep_idx_bearshort.py -- certify a DEDICATED StallBook clip pair for
IndexBearShort (NAS100 DON24 / US500 DON48) after the 2026-07-17 $400-giveback incident
(main book gate_pct=1.5 never armed at mfe 1.47%; feedback-profit-lock-mandatory).

Input: path CSVs from backtest/clip_path_idx_bearshort.cpp (REAL engine entry stream,
H1-close per-bar path entry->natural exit): trade_id,seq,exit_ms,dir,entry_px,px,
atr_pct,bull,cost_rt.

Simulates the EXACT StallCompanion.hpp PCT-gauge semantics (S-2026-07-17r code) at
H1-close grain per parent leg, $1/pt index CFD (sizing.hpp):
  - companion opens with the parent leg (mfe seeded at first-seen fav)
  - armed  = mfe_pct >= gate_pct
  - REVERSAL_CLIP when armed and fav <= mfe*(1-rev_gb)  -> bank upnl (minus companion RT cost)
  - LOSS_CUT_CLIP when upnl <= cold_loss_omega (-50)    -> bank upnl (minus cost)
  - after a clip the leg freezes until retrig: fav > peak*(1+retrig_pct) -> reopen (fresh mfe)
  - parent exit with companion open -> ENGINE_EXIT bank last upnl (minus cost)
  - stall_bars=9999 (pure giveback trail, turtle-clip precedent)
Companion is judged STANDALONE (own book, additive; never vs WIDE).

usage: stall_clip_sweep_idx_bearshort.py <path.csv> <LABEL> [--cell GATE GB]
"""
import sys, csv, math
from collections import defaultdict

STALL_BARS = 9999
RETRIG = 0.02       # turtle-clip precedent

def load(path):
    trades = defaultdict(list)
    with open(path) as f:
        r = csv.DictReader(f)
        for row in r:
            trades[int(row["trade_id"])].append((
                int(row["exit_ms"]), int(row["dir"]), float(row["entry_px"]),
                float(row["px"]), int(row["bull"]), float(row["cost_rt"])))
    return [trades[k] for k in sorted(trades)]

def sim_leg(rows, gate, gb, retrig=RETRIG, cold=-50.0):
    """One parent leg -> list of banked companion clips (usd, reason).

    HONEST own-contract accounting (BE-floor-on-open mandate: reclip=0 OR
    ANCHORED-reclip): a retrig re-entry banks upnl MINUS the upnl at reopen
    (anchor), never the full from-parent-entry upnl again. retrig=0 => one
    clip per parent leg (no anchor needed, matches raw StallBook banking).
    """
    entry = rows[0][2]; d = rows[0][1]
    cost_usd = rows[0][5] * entry          # cost_rt fraction * entry = $/leg RT ($1/pt)
    banks = []
    open_pos = False; mfe = 0.0; last_upnl = 0.0
    clipped = False; peak = 0.0; anchor = 0.0
    for (_, _, _, px, _, _) in rows:
        fav = (d * (px - entry) / entry) * 100.0   # % favourable
        upnl = d * (px - entry)                    # $ ( $1/pt )
        if clipped:
            if retrig > 0.0 and peak > 0.0 and fav > peak * (1.0 + retrig):
                clipped = False                    # retrig -> reopen ANCHORED here
                anchor = upnl
            else:
                continue
        if not open_pos:
            open_pos = True; mfe = fav
        if fav > mfe: mfe = fav
        last_upnl = upnl
        armed = mfe >= gate
        if armed and fav <= mfe * (1.0 - gb):
            banks.append((upnl - anchor - cost_usd, "REVERSAL_CLIP")); open_pos = False
            clipped = True; peak = mfe; continue
        if cold < 0.0 and upnl - anchor <= cold:
            banks.append((upnl - anchor - cost_usd, "LOSS_CUT_CLIP")); open_pos = False
            clipped = True; peak = mfe; continue
    if open_pos:
        banks.append((last_upnl - anchor - cost_usd, "ENGINE_EXIT"))
    return banks

def sim_leg_usd(rows, arm_usd, trail_usd, retrig_usd=0.0, cold=-50.0):
    """USD-gauge StallBook semantics (gold xau_tf*_usd precedent): armed when
    mfe_usd >= arm_usd; REVERSAL_CLIP when fav_usd <= mfe_usd - trail_usd.
    Anchored reclip (retrig_usd > 0: reopen when fav_usd > peak + retrig_usd)."""
    entry = rows[0][2]; d = rows[0][1]
    cost_usd = rows[0][5] * entry
    banks = []
    open_pos = False; mfe = 0.0; last_upnl = 0.0
    clipped = False; peak = 0.0; anchor = 0.0
    for (_, _, _, px, _, _) in rows:
        upnl = d * (px - entry)                    # $ ( $1/pt ) == fav_usd
        if clipped:
            if retrig_usd > 0.0 and peak > 0.0 and upnl > peak + retrig_usd:
                clipped = False; anchor = upnl
            else:
                continue
        if not open_pos:
            open_pos = True; mfe = upnl
        if upnl > mfe: mfe = upnl
        last_upnl = upnl
        armed = mfe >= arm_usd
        if armed and upnl <= mfe - trail_usd:
            banks.append((upnl - anchor - cost_usd, "REVERSAL_CLIP")); open_pos = False
            clipped = True; peak = mfe; continue
        if cold < 0.0 and upnl - anchor <= cold:
            banks.append((upnl - anchor - cost_usd, "LOSS_CUT_CLIP")); open_pos = False
            clipped = True; peak = mfe; continue
    if open_pos:
        banks.append((last_upnl - anchor - cost_usd, "ENGINE_EXIT"))
    return banks

def sim_leg_floored(rows, confirm_usd, trail_usd, retrig_usd=25.0, cost_mult=1.0):
    """BE-ENTRY FLOORED mimic (BeFloorOnOpenFoundation, mandatory for every NEW
    companion): the companion books NOTHING until fav_usd >= confirm_usd (>=2x RT
    cost); it then OPENS anchored at the parent entry (le=epx) with its stop
    FLOORED at BE (entry + cost). Trail clip banks at the H1 close that pierces
    max(mfe - trail_usd, floor) -- HONEST fill (actual close, real gap tail can
    book < BE; the LEVEL is >=BE by construction, the fill is whatever it is).
    Parent losers that never reach confirm are never mirrored (no -1R drag).
    Anchored reclip: reopen only when fav_usd > prior peak + retrig_usd; banks
    upnl minus the reopen anchor."""
    entry = rows[0][2]; d = rows[0][1]
    cost_usd = rows[0][5] * entry * cost_mult
    banks = []
    open_pos = False; mfe = 0.0; last_upnl = 0.0
    clipped = False; peak = 0.0; anchor = 0.0
    for (_, _, _, px, _, _) in rows:
        upnl = d * (px - entry)                    # $ ( $1/pt ) == fav_usd
        if clipped:
            # LEVEL-anchored reclip (restart-safe; the only state is peak):
            # reopen when upnl clears peak + retrig + confirm; anchor at the
            # LEVEL peak + retrig (never the crossing-bar overshoot).
            if retrig_usd > 0.0 and peak > 0.0 and upnl >= peak + retrig_usd + confirm_usd:
                clipped = False; anchor = peak + retrig_usd
                open_pos = True; mfe = upnl
            else:
                continue
        if not open_pos:
            if upnl < confirm_usd: continue        # BE-ENTRY: flat until confirmed
            open_pos = True; mfe = upnl
        if upnl > mfe: mfe = upnl
        last_upnl = upnl
        floor_lvl = anchor + cost_usd                  # BE floor (anchored)
        stop_lvl = max(mfe - trail_usd, floor_lvl)
        if upnl <= stop_lvl:
            banks.append((upnl - anchor - cost_usd, "FLOOR_CLIP")); open_pos = False
            clipped = True; peak = mfe; continue
    if open_pos:
        banks.append((last_upnl - anchor - cost_usd, "ENGINE_EXIT"))
    return banks

def stats(nets):
    n = len(nets); net = sum(nets)
    gp = sum(x for x in nets if x > 0); gl = -sum(x for x in nets if x < 0)
    pf = gp / gl if gl > 1e-9 else 999.0
    worst = min(nets) if nets else 0.0
    return n, net, pf, worst

def evaluate(legs, gate, gb, retrig=RETRIG, cold=-50.0, usd=False):
    per_leg = []          # (t0, bull, [banks])
    for rows in legs:
        bs = (sim_leg_floored(rows, gate, gb, retrig, cold) if usd == "floor"
              else sim_leg_usd(rows, gate, gb, retrig, cold) if usd
              else sim_leg(rows, gate, gb, retrig, cold))
        per_leg.append((rows[0][0], rows[0][4], bs))
    all_banks = [b for (_, _, bs) in per_leg for (b, _) in bs]
    n, net, pf, worst = stats(all_banks)
    mid = len(per_leg) // 2
    h1 = [b for (_, _, bs) in per_leg[:mid] for (b, _) in bs]
    h2 = [b for (_, _, bs) in per_leg[mid:] for (b, _) in bs]
    bear = [b for (_, bu, bs) in per_leg if bu == 0 for (b, _) in bs]
    bull = [b for (_, bu, bs) in per_leg if bu == 1 for (b, _) in bs]
    return dict(n=n, net=net, pf=pf, worst=worst,
                h1=sum(h1), h2=sum(h2), bear=sum(bear), bull=sum(bull),
                nbull=len(bull))

def main():
    path, label = sys.argv[1], sys.argv[2]
    legs = load(path)
    print(f"[{label}] {len(legs)} parent legs")
    if len(sys.argv) > 3 and sys.argv[3] == "--cell":
        g, gb = float(sys.argv[4]), float(sys.argv[5])
        r = evaluate(legs, g, gb)
        print(f"  cell gate={g} gb={gb}: {r}")
        return
    # ── BE-ENTRY FLOORED mimic (BeFloorOnOpenFoundation) ──
    for retrig_usd in (0.0, 25.0):
        for cm in (1.0, 2.0):
            print(f"-- FLOORED be-entry retrig_usd={retrig_usd} cost_x{cm:.0f} --")
            print(f"{'cnf$':>5} {'tr$':>5} | {'n':>4} {'net$':>8} {'PF':>6} {'worst':>7} "
                  f"{'WF-H1':>8} {'WF-H2':>8} verdict")
            for cnf in (10.0, 25.0, 50.0):
                for tr in (25.0, 50.0, 75.0, 100.0):
                    r = evaluate(legs, cnf, tr, retrig_usd, cm, usd="floor")
                    ok = (r["net"] > 0 and r["pf"] >= 1.3 and r["h1"] > 0 and r["h2"] > 0)
                    v = "PASS" if ok else "fail"
                    print(f"{cnf:5.0f} {tr:5.0f} | {r['n']:4d} {r['net']:8.1f} {r['pf']:6.2f} "
                          f"{r['worst']:7.1f} {r['h1']:8.1f} {r['h2']:8.1f} {v}")
    # ── USD gauge (gold xau_tf*_usd precedent shape) ──
    arm_grid   = [float(x) for x in (25, 50, 75, 100, 150)]
    trail_grid = [float(x) for x in (25, 50, 75, 100, 150)]
    for retrig_usd in (0.0, 25.0):
        for cold in (-50.0, -150.0, -300.0, 0.0):
            print(f"-- USD gauge retrig_usd={retrig_usd} cold_loss={'OFF' if cold == 0 else cold} --")
            print(f"{'arm$':>5} {'tr$':>5} | {'n':>4} {'net$':>8} {'PF':>6} {'worst':>7} "
                  f"{'WF-H1':>8} {'WF-H2':>8} verdict")
            for arm in arm_grid:
                for tr in trail_grid:
                    r = evaluate(legs, arm, tr, retrig_usd, cold, usd=True)
                    ok = (r["net"] > 0 and r["pf"] >= 1.3 and r["h1"] > 0 and r["h2"] > 0)
                    v = "PASS" if ok else "fail"
                    print(f"{arm:5.0f} {tr:5.0f} | {r['n']:4d} {r['net']:8.1f} {r['pf']:6.2f} "
                          f"{r['worst']:7.1f} {r['h1']:8.1f} {r['h2']:8.1f} {v}")
    for retrig in (0.0, RETRIG):
        for cold in (-50.0, -150.0, -300.0, 0.0):
            print(f"-- retrig={retrig} ({'reclip OFF' if retrig == 0 else 'ANCHORED reclip'}) "
                  f"cold_loss={'OFF' if cold == 0 else cold} --")
            print(f"{'gate':>5} {'gb':>4} | {'n':>4} {'net$':>8} {'PF':>6} {'worst':>7} "
                  f"{'WF-H1':>8} {'WF-H2':>8} verdict")
            for gate in (0.25, 0.5, 0.75, 1.0, 1.25, 1.5):
                row=[]
                for gb in (0.3, 0.4, 0.5, 0.6, 0.7, 0.8):
                    r = evaluate(legs, gate, gb, retrig, cold)
                    ok = (r["net"] > 0 and r["pf"] >= 1.3 and r["h1"] > 0 and r["h2"] > 0)
                    v = "PASS" if ok else "fail"
                    print(f"{gate:5.2f} {gb:4.1f} | {r['n']:4d} {r['net']:8.1f} {r['pf']:6.2f} "
                          f"{r['worst']:7.1f} {r['h1']:8.1f} {r['h2']:8.1f} {v}")

if __name__ == "__main__":
    main()
